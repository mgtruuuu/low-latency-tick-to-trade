/**
 * @file strategy_thread.hpp
 * @brief Strategy thread — drains SPSC queue for market data, evaluates
 *        strategy, manages orders via TCP to the exchange.
 *
 * Lifecycle: construct (cold) -> start() (spawns thread) -> join().
 * The thread exits when the external stop flag becomes non-zero.
 *
 * Design:
 *   - Header-only, zero allocation on hot path.
 *   - Owns std::thread + loop-local state (OrderResponseHandler,
 *     OrderSendHandler, TCP rx cursors).
 *   - Takes references to externally-owned state.
 *   - stop_flag/kill_switch_flag are references to std::atomic_flag.
 */

#pragma once

#include "md_types.hpp"
#include "order_manager.hpp"
#include "order_response_handler.hpp"
#include "order_send_handler.hpp"
#include "strategy_policy.hpp"
#include "tcp_connection.hpp"
#include "latency_tracker.hpp"

#include "shared/protocol.hpp"
#include "shared/protocol_codec.hpp"

#include "net/epoll_wrapper.hpp"
#include "net/message_codec.hpp"
#include "net/scoped_fd.hpp"
#include "net/tcp_socket.hpp"

#include "pipeline_log_push.hpp"

#include "sys/log/signal_logger.hpp"
#include "sys/nano_clock.hpp"
#include "sys/thread/affinity.hpp"
#include "sys/thread/hot_path_control.hpp"

#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <span>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <thread>
#include <utility>

namespace mk::app {

/// Strategy thread: drains SPSC queue for market data, evaluates strategy,
/// manages TCP order flow to the exchange.
///
/// Template parameter StrategyT must satisfy StrategyPolicy concept.
///
/// Non-copyable, non-movable (holds references + owns thread).
template <StrategyPolicy StrategyT>
class StrategyThread {
public:
  /// Construction parameters. All references must outlive the StrategyThread.
  struct Config {
    // Core pipeline
    MdToStrategyQueue &md_queue;
    net::TcpSocket &tcp_sock;
    net::EpollWrapper &epoll;
    StrategyT &strategy;
    OrderManager &order_mgr;
    LatencyTracker &tracker;
    // TCP buffers (from StrategyCtx)
    std::span<std::byte> tcp_tx_buf;
    std::span<std::byte> scratch;
    char *tcp_rx_data{nullptr};
    std::size_t tcp_rx_size{0};
    // Connection
    ConnectionState &conn;
    const char *exchange_host{nullptr};
    std::uint16_t exchange_port{0};
    // Control
    std::atomic_flag &stop_flag;
    std::atomic_flag &kill_switch_flag;
    std::int32_t pin_core{-1};
    std::int64_t stats_interval_ns{0};
    // Logging
    PipelineLogQueue &log_queue;
  };

  explicit StrategyThread(const Config &cfg) noexcept
      : md_queue_(cfg.md_queue), tcp_sock_(cfg.tcp_sock), epoll_(cfg.epoll),
        strategy_(cfg.strategy), order_mgr_(cfg.order_mgr),
        tracker_(cfg.tracker), tcp_tx_buf_(cfg.tcp_tx_buf),
        scratch_(cfg.scratch), tcp_rx_data_(cfg.tcp_rx_data),
        tcp_rx_size_(cfg.tcp_rx_size), conn_(cfg.conn),
        exchange_host_(cfg.exchange_host), exchange_port_(cfg.exchange_port),
        stop_flag_(cfg.stop_flag), kill_switch_flag_(cfg.kill_switch_flag),
        pin_core_(cfg.pin_core), stats_interval_ns_(cfg.stats_interval_ns),
        log_queue_(cfg.log_queue) {}

  // Non-copyable, non-movable (holds references + owns thread).
  StrategyThread(const StrategyThread &) = delete;
  StrategyThread &operator=(const StrategyThread &) = delete;
  StrategyThread(StrategyThread &&) = delete;
  StrategyThread &operator=(StrategyThread &&) = delete;

  ~StrategyThread() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  /// Spawn the Strategy thread. Must be called exactly once.
  void start() { thread_ = std::thread(&StrategyThread::run, this); }

  /// Block until the Strategy thread exits.
  void join() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  // -- Observers (for shutdown diagnostics) --

  [[nodiscard]] const OrderSendHandler &send_handler() const noexcept {
    return send_handler_;
  }
  [[nodiscard]] const OrderResponseHandler &
  response_handler() const noexcept {
    return response_handler_;
  }

private:
  /// The event loop — runs on the spawned thread.
  void run() noexcept {
    // Pin to the Strategy-dedicated core.
    if (pin_core_ >= 0) {
      auto result = sys::thread::pin_current_thread(
          static_cast<std::uint32_t>(pin_core_));
      if (result == 0) {
        sys::log::signal_log("[STRATEGY] Pinned to core ", pin_core_, '\n');
        auto numa = sys::thread::get_cpu_numa_node(
            static_cast<std::uint32_t>(pin_core_));
        if (numa >= 0) {
          sys::log::signal_log("[STRATEGY] Core ", pin_core_, " → NUMA node ",
                               numa, '\n');
        }
      } else {
        sys::log::signal_log("[STRATEGY] WARNING: pin failed: ",
                             strerror(result), '\n');
      }
    }

    sys::thread::set_hot_path_mode(true);
    sys::log::signal_log("[STRATEGY] Entering strategy loop\n");

    // Periodic stats dump.
    std::int64_t last_stats_dump = sys::monotonic_nanos();

    // Drain batch buffer — on stack, sized for one batch cycle.
    // Matches kMdBatchSize in main.cpp so consumer drains at producer rate.
    constexpr std::size_t kDrainBatch = 64;
    QueuedUpdate batch[kDrainBatch]; // NOLINT(cppcoreguidelines-avoid-c-arrays)

    // Epoll for TCP events only (UDP is on the MD thread).
    std::array<struct epoll_event, 8> events{};

    while (!stop_flag_.test(std::memory_order_relaxed)) {
      // -- Kill switch logic --
      if (kill_switch_flag_.test(std::memory_order_relaxed) &&
          !order_mgr_.is_killed() &&
          conn_.state == TcpState::kConnected) [[unlikely]] {
        auto ks_batch = order_mgr_.trigger_kill_switch();
        for (std::uint32_t ci = 0; ci < ks_batch.count; ++ci) {
          auto plen =
              serialize_cancel_order(scratch_, ks_batch.cancels[ci]);
          if (plen > 0) [[likely]] {
            auto tlen = pack_tcp_message(
                tcp_tx_buf_, MsgType::kCancelOrder,
                std::span{scratch_.data(), plen});
            if (tlen > 0) [[likely]] {
              auto result = tcp_sock_.send_nonblocking(
                  reinterpret_cast<const char *>(tcp_tx_buf_.data()), tlen);
              if (!check_send_result(result, conn_, "KillSwitchCancel"))
                  [[unlikely]] {
                disconnect_and_reconnect(tcp_sock_, epoll_, conn_,
                                         tcp_rx_read_, tcp_rx_write_,
                                         "Send failed during kill switch");
                break;
              }
            }
          }
        }
      }

      if (order_mgr_.kill_switch_state() ==
          OrderManager::KillSwitchState::kComplete) [[unlikely]] {
        stop_flag_.test_and_set(std::memory_order_relaxed);
        continue;
      }

      // -- Heartbeat / reconnection --
      if (conn_.state == TcpState::kConnected) {
        const auto now = sys::monotonic_nanos();

        if (now - conn_.last_hb_sent >= kHeartbeatIntervalNs) {
          auto hb_len = pack_tcp_message(
              tcp_tx_buf_, MsgType::kHeartbeat, {});
          if (hb_len > 0) [[likely]] {
            auto result = tcp_sock_.send_nonblocking(
                reinterpret_cast<const char *>(tcp_tx_buf_.data()), hb_len);
            if (!check_send_result(result, conn_, "Heartbeat")) [[unlikely]] {
              disconnect_and_reconnect(
                  tcp_sock_, epoll_, conn_, tcp_rx_read_, tcp_rx_write_,
                  "Heartbeat send failed — reconnecting");
            }
          }
          conn_.last_hb_sent = now;
          conn_.heartbeats_sent.fetch_add(1, std::memory_order_relaxed);
          (void)log_connection(
              log_queue_, kThreadIdStrategy,
              LogLevel::kDebug,
              ConnectionEvent::kHeartbeatSent);
        }

        if (now - conn_.last_hb_recv >= kHeartbeatTimeoutNs) [[unlikely]] {
          (void)log_connection(
              log_queue_, kThreadIdStrategy,
              LogLevel::kWarn,
              ConnectionEvent::kDisconnect);
          disconnect_and_reconnect(tcp_sock_, epoll_, conn_, tcp_rx_read_,
                                   tcp_rx_write_,
                                   "Heartbeat timeout — reconnecting");
        }
      }

      // Async connect timeout — if EPOLLOUT hasn't arrived within
      // kConnectTimeoutNs, close the pending fd and fall through to
      // kDisconnected for normal reconnect scheduling.
      if (conn_.state == TcpState::kConnecting) {
        const auto now = sys::monotonic_nanos();
        if (std::cmp_greater_equal(now, conn_.connect_deadline)) {
          sys::log::signal_log(
              "[PIPELINE] Connect timeout — closing pending fd\n");
          epoll_.remove(conn_.connecting_fd);
          ::close(conn_.connecting_fd);
          conn_.connecting_fd = -1;
          conn_.state = TcpState::kDisconnected;
          schedule_reconnect(conn_, now, "connect timed out");
        }
      }

      if (conn_.state == TcpState::kDisconnected) {
        const auto now = sys::monotonic_nanos();
        if (std::cmp_greater_equal(now, conn_.next_reconnect_at)) {
          if (conn_.reconnect_attempts >= kMaxReconnectAttempts) [[unlikely]] {
            sys::log::signal_log(
                "[STRATEGY] Max reconnect attempts — stopping\n");
            stop_flag_.test_and_set(std::memory_order_relaxed);
            continue;
          }

          net::ScopedFd guard(
              ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
          if (!guard) {
            schedule_reconnect(conn_, now, strerror(errno));
          } else {
            const int new_fd = guard.get();
            (void)::setsockopt(new_fd, SOL_SOCKET, SO_SNDBUF, &kTcpSndBufSize,
                               sizeof(kTcpSndBufSize));
            (void)::setsockopt(new_fd, SOL_SOCKET, SO_RCVBUF, &kTcpRcvBufSize,
                               sizeof(kTcpRcvBufSize));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(exchange_port_);
            inet_pton(AF_INET, exchange_host_, &addr.sin_addr);

            const int rc = ::connect(
                new_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
            if (rc == 0) {
              if (!finalize_tcp_connect(guard.release(), tcp_sock_, epoll_,
                                        conn_)) {
                schedule_reconnect(conn_, now, "socket option setup failed");
              }
            } else if (errno == EINPROGRESS) {
              conn_.connecting_fd = guard.release();
              epoll_.add(conn_.connecting_fd, EPOLLOUT, &conn_);
              conn_.state = TcpState::kConnecting;
              conn_.connect_deadline = now + kConnectTimeoutNs;
            } else {
              schedule_reconnect(conn_, now, strerror(errno));
            }
          }
        }
      }

      // -- Order timeout check --
      if (conn_.state == TcpState::kConnected) {
        auto timeout_batch =
            order_mgr_.advance_timeouts(sys::monotonic_nanos());
        for (std::uint32_t ti = 0; ti < timeout_batch.count; ++ti) {
          auto plen =
              serialize_cancel_order(scratch_, timeout_batch.cancels[ti]);
          if (plen > 0) [[likely]] {
            auto tlen = pack_tcp_message(
                tcp_tx_buf_, MsgType::kCancelOrder,
                std::span{scratch_.data(), plen});
            if (tlen > 0) [[likely]] {
              auto result = tcp_sock_.send_nonblocking(
                  reinterpret_cast<const char *>(tcp_tx_buf_.data()), tlen);
              if (!check_send_result(result, conn_, "TimeoutCancel"))
                  [[unlikely]] {
                disconnect_and_reconnect(
                    tcp_sock_, epoll_, conn_, tcp_rx_read_, tcp_rx_write_,
                    "Send failed during timeout cancel");
                break;
              }
            }
          }
        }
      }

      // -- Periodic stats dump --
      if (stats_interval_ns_ > 0) {
        const auto now = sys::monotonic_nanos();
        if (now - last_stats_dump >= stats_interval_ns_) [[unlikely]] {
          last_stats_dump = now;
          sys::log::signal_log(
              "[STATS] ticks:", strategy_.signals_generated(),
              " orders:", order_mgr_.orders_sent(),
              " fills:", order_mgr_.fills_received(),
              " rejects:", order_mgr_.rejects_received(),
              " timeouts:", order_mgr_.timeouts_fired(), '\n');
        }
      }

      // -- Drain SPSC queue: process market data from MD thread --
      {
        auto count = md_queue_.drain(batch);
        // Timestamp immediately after drain — same for all items in this batch.
        // Used to measure pure queue wait (t_drain - t0), excluding
        // batch-internal processing accumulation that inflates per-item
        // t_dequeue timestamps.
        const auto t_drain = sys::rdtsc();

        for (std::size_t j = 0; j < count; ++j) {
          const auto &queued = batch[j];
          const auto t0 = queued.recv_tsc;
          auto t_dequeue = sys::rdtsc();

          // Record SPSC hop latency (MD thread push -> Strategy thread pop).
          // Note: t_dequeue includes processing time of batch[0..j-1].
          if (t_dequeue > t0) {
            const auto hop_cycles = t_dequeue - t0;
            tracker_.record_queue_latency(hop_cycles);
            (void)log_latency(
                log_queue_, kThreadIdStrategy,
                LatencyStage::kQueueHop, hop_cycles, t0);
          }

          // Record pure queue wait (t_drain - t0).
          // Same t_drain for all items in this batch — isolates how long
          // data actually waited in the queue without batch processing noise.
          if (t_drain > t0) {
            tracker_.record_queue_wait(t_drain - t0);
          }

          // Strategy evaluation.
          Signal signal;
          const bool has_signal =
              strategy_.on_market_data(queued.update, signal);

#ifdef PROFILE_STAGES
          {
            auto t2 = sys::rdtsc();
            auto strat_cycles = t2 - t_dequeue;
            tracker_.record_strategy(strat_cycles);
            (void)log_latency(
                log_queue_, kThreadIdStrategy,
                LatencyStage::kStrategy, strat_cycles, t0);
          }
#endif

          if (!has_signal) {
            continue;
          }

          if (conn_.state != TcpState::kConnected) [[unlikely]] {
            continue;
          }

          if (!send_handler_.on_signal(signal, order_mgr_, tracker_, tcp_sock_,
                                       scratch_, tcp_tx_buf_, t0, conn_,
                                       log_queue_))
              [[unlikely]] {
            disconnect_and_reconnect(
                tcp_sock_, epoll_, conn_, tcp_rx_read_, tcp_rx_write_,
                "Send failed in order path — reconnecting");
            break; // Stop processing batch — connection lost.
          }
        }
      }

      // -- TCP response path (epoll with timeout=0 for non-blocking check) --
      const int tcp_n = epoll_.wait(events, 0);

      for (int i = 0; i < tcp_n; ++i) {
        // data.ptr always points to our ConnectionState — dispatch by state.
        auto *c = static_cast<ConnectionState *>(events[i].data.ptr);
        assert(c != nullptr);

        // Async connect completion (EPOLLOUT on connecting fd).
        if (c->state == TcpState::kConnecting &&
            (events[i].events & EPOLLOUT)) {
          const int cfd = c->connecting_fd;
          net::ScopedFd fd_guard(cfd);
          c->connecting_fd = -1;

          int sock_err = 0;
          socklen_t err_len = sizeof(sock_err);
          getsockopt(cfd, SOL_SOCKET, SO_ERROR, &sock_err, &err_len);
          epoll_.remove(cfd);

          if (sock_err == 0 &&
              finalize_tcp_connect(fd_guard.release(), tcp_sock_, epoll_, *c)) {
            // Connected.
          } else {
            c->state = TcpState::kDisconnected;
            schedule_reconnect(*c, sys::monotonic_nanos(),
                               sock_err != 0 ? strerror(sock_err)
                                             : "socket option setup failed");
          }
          continue;
        }

        // TCP data (order responses).
        if (c->state == TcpState::kConnected && (events[i].events & EPOLLIN)) {
          while (true) {
            // Partial message at buffer tail — compact parsed bytes out
            // to make room for more recv().
            if (tcp_rx_write_ >= tcp_rx_size_) {
              if (tcp_rx_read_ > 0) {
                const auto remaining = tcp_rx_write_ - tcp_rx_read_;
                std::memmove(tcp_rx_data_, tcp_rx_data_ + tcp_rx_read_,
                             remaining);
                tcp_rx_read_ = 0;
                tcp_rx_write_ = remaining;
              } else {
                // Single message larger than tcp_rx_size — cannot make
                // progress. Silently resetting would desync the stream.
                disconnect_and_reconnect(
                    tcp_sock_, epoll_, conn_, tcp_rx_read_, tcp_rx_write_,
                    "TCP rx buffer overflow — reconnecting");
                break;
              }
            }

            auto result = tcp_sock_.receive_nonblocking(
                tcp_rx_data_ + tcp_rx_write_, tcp_rx_size_ - tcp_rx_write_);

            if (result.status == net::TcpSocket::RecvStatus::kWouldBlock) {
              break;
            }
            if (result.status == net::TcpSocket::RecvStatus::kPeerClosed ||
                result.status == net::TcpSocket::RecvStatus::kError) {
              const char *reason =
                  result.status == net::TcpSocket::RecvStatus::kPeerClosed
                      ? "Exchange disconnected — reconnecting"
                      : "TCP recv error — reconnecting";
              disconnect_and_reconnect(tcp_sock_, epoll_, conn_, tcp_rx_read_,
                                       tcp_rx_write_, reason);
              break;
            }

            tcp_rx_write_ += static_cast<std::size_t>(result.bytes_received);
            (void)tcp_sock_.set_tcp_quickack(true);

            while (tcp_rx_write_ - tcp_rx_read_ >= net::kMessageHeaderSize) {
              auto view = std::as_bytes(std::span<const char>{
                  tcp_rx_data_ + tcp_rx_read_,
                  tcp_rx_write_ - tcp_rx_read_});

              net::ParsedMessageView msg;
              if (!net::unpack_message(view, msg)) {
                break;
              }

              const std::size_t consumed =
                  net::kMessageHeaderSize + msg.header.payload_len;
              if (response_handler_.on_tcp_message(msg, order_mgr_, tracker_,
                                                   conn_, log_queue_)) {
                kill_switch_flag_.test_and_set(std::memory_order_relaxed);
              }
              tcp_rx_read_ += consumed;
            }

            if (tcp_rx_read_ == tcp_rx_write_) {
              tcp_rx_read_ = 0;
              tcp_rx_write_ = 0;
            }
          }
          continue;
        }

        // TCP errors/hangups.
        if (c->state == TcpState::kConnected &&
            (events[i].events & (EPOLLHUP | EPOLLERR))) {
          disconnect_and_reconnect(tcp_sock_, epoll_, *c, tcp_rx_read_,
                                   tcp_rx_write_,
                                   "Exchange connection lost — reconnecting");
          continue;
        }
      }
    }

    sys::thread::set_hot_path_mode(false);
    sys::log::signal_log("[STRATEGY] Strategy loop exited\n");
  }

  // -- References to externally-owned state --
  MdToStrategyQueue &md_queue_;
  net::TcpSocket &tcp_sock_;
  net::EpollWrapper &epoll_;
  StrategyT &strategy_;
  OrderManager &order_mgr_;
  LatencyTracker &tracker_;
  std::span<std::byte> tcp_tx_buf_;
  std::span<std::byte> scratch_;
  char *tcp_rx_data_;
  std::size_t tcp_rx_size_;
  ConnectionState &conn_;
  const char *exchange_host_;
  std::uint16_t exchange_port_;
  std::atomic_flag &stop_flag_;
  std::atomic_flag &kill_switch_flag_;
  std::int32_t pin_core_;
  std::int64_t stats_interval_ns_;
  PipelineLogQueue &log_queue_;

  // -- Loop-local state (owned — formerly local variables) --
  std::size_t tcp_rx_read_{0};
  std::size_t tcp_rx_write_{0};
  OrderResponseHandler response_handler_;
  OrderSendHandler send_handler_;

  // -- Thread --
  std::thread thread_;
};

} // namespace mk::app
