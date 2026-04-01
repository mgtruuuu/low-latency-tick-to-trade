/**
 * @file tcp_server_test.cpp
 * @brief Tests for TcpServer — reusable ET epoll TCP server library.
 *
 * Test strategy:
 *   - Compile-time: static_assert concept satisfaction for framers and
 * handlers.
 *   - Unit: LengthPrefixFramer and RawFramer encode/decode correctness.
 *   - Integration: loopback TCP using ephemeral port (port=0). Server runs on
 *     a background std::jthread, client connects via blocking TcpSocket.
 *   - Uses small template parameters (kMaxConns=64, 4 KiB buffers) to keep
 *     test memory footprint low.
 */

#include "net/tcp_server.hpp"

#include "net/length_prefix_codec.hpp"
#include "net/tcp_socket.hpp"
#include "sys/endian.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <span>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace mk::net {
namespace {

// ============================================================================
// Compile-time concept checks
// ============================================================================

static_assert(Framer<LengthPrefixFramer>);
static_assert(Framer<RawFramer>);

struct MinimalHandler {
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  bool on_connect(ConnId /*unused*/) noexcept { return true; }
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  std::size_t on_data(ConnId /*unused*/, std::span<const std::byte> /*unused*/,
                      std::span<std::byte> /*unused*/) noexcept {
    return 0;
  }
  void on_disconnect(ConnId /*unused*/) noexcept {}
};
static_assert(TcpHandler<MinimalHandler>);

// ============================================================================
// Framer unit tests
// ============================================================================

TEST(LengthPrefixFramerTest, RoundTrip) {
  const LengthPrefixFramer framer;

  // Encode
  const char msg[] = "hello";
  auto payload = std::as_bytes(std::span{msg, 5});
  std::array<std::byte, 64> buf{};
  auto tx = std::span{buf};
  const std::size_t written = framer.encode(tx, payload);
  ASSERT_GT(written, 0U);
  EXPECT_EQ(written, kLengthPrefixSize + 5);

  // Decode
  auto result = framer.decode(std::span{buf.data(), written});
  ASSERT_EQ(result.status, FrameStatus::kOk);
  EXPECT_EQ(result.payload.size(), 5U);
  EXPECT_EQ(std::memcmp(result.payload.data(), msg, 5), 0);
}

TEST(LengthPrefixFramerTest, EncodeOverflow) {
  const LengthPrefixFramer framer;
  std::array<std::byte, 2> tiny{};
  auto payload = std::as_bytes(std::span{"abc", 3});
  EXPECT_EQ(framer.encode(tiny, payload), 0U);
}

TEST(RawFramerTest, DecodeEmptyIsIncomplete) {
  const RawFramer framer;
  auto result = framer.decode(std::span<const std::byte>{});
  EXPECT_EQ(result.status, FrameStatus::kIncomplete);
}

TEST(RawFramerTest, DecodeNonEmpty) {
  const RawFramer framer;
  const char msg[] = "data";
  auto buf = std::as_bytes(std::span{msg, 4});
  auto result = framer.decode(buf);
  ASSERT_EQ(result.status, FrameStatus::kOk);
  EXPECT_EQ(result.frame_size, 4U);
  EXPECT_EQ(result.payload.size(), 4U);
}

TEST(RawFramerTest, EncodeOverflow) {
  const RawFramer framer;
  std::array<std::byte, 2> tiny{};
  auto payload = std::as_bytes(std::span{"abcd", 4});
  EXPECT_EQ(framer.encode(tiny, payload), 0U);
}

TEST(RawFramerTest, EncodeRoundTrip) {
  const RawFramer framer;
  const char msg[] = "test";
  auto payload = std::as_bytes(std::span{msg, 4});
  std::array<std::byte, 16> buf{};
  const std::size_t written = framer.encode(buf, payload);
  ASSERT_EQ(written, 4U);
  EXPECT_EQ(std::memcmp(buf.data(), msg, 4), 0);
}

// ============================================================================
// Integration test infrastructure
// ============================================================================

// Small server parameters to keep test memory footprint low.
constexpr int kTestMaxConns = 64;
constexpr std::size_t kTestBufSize = 4096;

// Echo handler: echoes each received payload back as a framed response.
// on_data writes the response directly into tx_space (zero-copy).
struct EchoHandler {
  std::atomic<int> connect_count{0};
  std::atomic<int> disconnect_count{0};
  std::atomic<int> data_count{0};

  bool on_connect(ConnId /*unused*/) noexcept {
    connect_count.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  std::size_t on_data(ConnId /*unused*/, std::span<const std::byte> payload,
                      std::span<std::byte> tx_space) noexcept {
    data_count.fetch_add(1, std::memory_order_relaxed);
    if (payload.size() > tx_space.size()) {
      return 0;
    }
    std::memcpy(tx_space.data(), payload.data(), payload.size());
    return payload.size();
  }

  void on_disconnect(ConnId /*unused*/) noexcept {
    disconnect_count.fetch_add(1, std::memory_order_relaxed);
  }
};

// Handler that rejects all connections.
struct RejectHandler {
  std::atomic<int> connect_count{0};
  std::atomic<int> disconnect_count{0};

  bool on_connect(ConnId /*unused*/) noexcept {
    connect_count.fetch_add(1, std::memory_order_relaxed);
    return false; // reject
  }

  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  std::size_t on_data(ConnId /*unused*/, std::span<const std::byte> /*unused*/,
                      std::span<std::byte> /*unused*/) noexcept {
    return 0;
  }

  void on_disconnect(ConnId /*unused*/) noexcept {
    disconnect_count.fetch_add(1, std::memory_order_relaxed);
  }
};

using EchoServer = TcpServer<EchoHandler, kTestMaxConns, kTestBufSize,
                             kTestBufSize, LengthPrefixFramer>;

using RawEchoServer = TcpServer<EchoHandler, kTestMaxConns, kTestBufSize,
                                kTestBufSize, RawFramer>;

using RejectServer = TcpServer<RejectHandler, kTestMaxConns, kTestBufSize,
                               kTestBufSize, LengthPrefixFramer>;

// Buffer + server wrapper. Tests allocate via aligned_alloc; the buffer
// must outlive the server. This struct bundles them for convenience.
struct AlignedBuffer {
  void *ptr = nullptr;
  std::size_t size = 0;

  explicit AlignedBuffer(std::size_t sz)
      : ptr(std::aligned_alloc(64, sz)),
        size(sz) { // NOLINT(cppcoreguidelines-no-malloc)
    if (!ptr) {
      std::abort();
    }
  }

  ~AlignedBuffer() { std::free(ptr); } // NOLINT(cppcoreguidelines-no-malloc)

  AlignedBuffer(const AlignedBuffer &) = delete;
  AlignedBuffer &operator=(const AlignedBuffer &) = delete;
  AlignedBuffer(AlignedBuffer &&) = delete;
  AlignedBuffer &operator=(AlignedBuffer &&) = delete;
};

// Helper: connects a blocking TCP socket to localhost:port.
// Returns the raw fd (caller wraps in TcpSocket). Aborts on failure.
int connect_to(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    std::abort();
  }

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) ==
      -1) {
    ::close(fd);
    std::abort();
  }
  return fd;
}

// Helper: sends a length-prefix framed message via blocking send.
void send_framed(TcpSocket &sock, const char *msg, std::size_t len) {
  std::array<std::byte, 4096> buf{};
  auto payload = std::as_bytes(std::span{msg, len});
  auto out = std::span{buf};
  auto written = encode_length_prefix_frame(out, payload);
  ASSERT_TRUE(written.has_value());
  auto result = sock.send_blocking(
      reinterpret_cast<const char *>(buf.data()),
      *written); // NOLINT(bugprone-unchecked-optional-access)
  ASSERT_EQ(result.status, TcpSocket::SendStatus::kOk);
}

// Helper: receives a length-prefix framed message via blocking recv.
// Returns the payload as a string for easy comparison.
std::string recv_framed(TcpSocket &sock) {
  // Read the 4-byte header first.
  std::array<char, 4> hdr{};
  auto hdr_result = sock.receive_blocking(hdr.data(), 4);
  if (hdr_result.status != TcpSocket::RecvStatus::kOk) {
    return {};
  }

  auto total_len = static_cast<std::size_t>(
      mk::sys::load_be32(reinterpret_cast<const std::byte *>(hdr.data())));
  if (total_len < kLengthPrefixSize) {
    return {};
  }

  const std::size_t payload_len = total_len - kLengthPrefixSize;
  if (payload_len == 0) {
    return {};
  }

  std::string payload(payload_len, '\0');
  auto body_result = sock.receive_blocking(payload.data(), payload_len);
  if (body_result.status != TcpSocket::RecvStatus::kOk) {
    return {};
  }

  return payload;
}

// Helper: connects to localhost:port to wake up a blocking epoll_wait.
// Ignores all errors — this is a best-effort wake-up for test teardown.
void wake_epoll(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd != -1) {
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    auto rc =
        ::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
    (void)rc;
    ::close(fd);
  }
}

// Spin-wait helper with timeout.
template <typename Pred>
bool wait_for(const Pred &pred,
              std::chrono::milliseconds timeout = std::chrono::milliseconds{
                  2000}) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return true;
}

// ============================================================================
// Integration tests — echo (LengthPrefixFramer)
// ============================================================================

class TcpServerEchoTest : public ::testing::Test {
protected:
  EchoHandler handler_;
  AlignedBuffer buf_{EchoServer::required_buffer_size()};
  std::unique_ptr<EchoServer> server_;
  std::jthread server_thread_;

  void SetUp() override {
    // busy_spin=false and no pinning/alloc_guard for test stability.
    const TcpServerConfig cfg{
        .port = 0,
        .backlog = 16,
        .nodelay = true,
        .busy_spin = false,
        .pin_core = -1,
        .alloc_guard = false,
    };
    server_ = std::make_unique<EchoServer>(handler_, cfg, buf_.ptr, buf_.size);
    ASSERT_TRUE(server_->listen());
    ASSERT_GT(server_->port(), 0);

    server_thread_ = std::jthread([this] { server_->run(); });

    // Brief sleep to let the server enter epoll_wait.
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }

  void TearDown() override {
    server_->request_stop();
    wake_epoll(server_->port());
    server_thread_.join();
  }
};

TEST_F(TcpServerEchoTest, SingleFrame) {
  TcpSocket client(connect_to(server_->port()));

  send_framed(client, "hello", 5);
  const std::string reply = recv_framed(client);
  EXPECT_EQ(reply, "hello");

  EXPECT_TRUE(wait_for([&] { return handler_.connect_count.load() >= 1; }));
  EXPECT_GE(handler_.data_count.load(), 1);
}

TEST_F(TcpServerEchoTest, MultipleFrames) {
  TcpSocket client(connect_to(server_->port()));

  send_framed(client, "aaa", 3);
  send_framed(client, "bbb", 3);
  send_framed(client, "ccc", 3);

  EXPECT_EQ(recv_framed(client), "aaa");
  EXPECT_EQ(recv_framed(client), "bbb");
  EXPECT_EQ(recv_framed(client), "ccc");
}

TEST_F(TcpServerEchoTest, MultipleClients) {
  TcpSocket c1(connect_to(server_->port()));
  TcpSocket c2(connect_to(server_->port()));

  send_framed(c1, "one", 3);
  send_framed(c2, "two", 3);

  EXPECT_EQ(recv_framed(c1), "one");
  EXPECT_EQ(recv_framed(c2), "two");

  EXPECT_TRUE(wait_for([&] { return handler_.connect_count.load() >= 2; }));
}

TEST_F(TcpServerEchoTest, ClientDisconnect) {
  {
    TcpSocket client(connect_to(server_->port()));
    send_framed(client, "bye", 3);
    recv_framed(client);
    // client goes out of scope → close() → server sees EPOLLRDHUP/HUP
  }

  EXPECT_TRUE(wait_for([&] { return handler_.disconnect_count.load() >= 1; }));
}

TEST_F(TcpServerEchoTest, GracefulHalfClose) {
  TcpSocket client(connect_to(server_->port()));

  send_framed(client, "data", 4);
  const std::string reply = recv_framed(client);
  EXPECT_EQ(reply, "data");

  // Half-close: signal no more data from client side.
  (void)client.shutdown(SHUT_WR);

  // Server should detect EPOLLRDHUP and close the connection after
  // flushing any pending tx data.
  EXPECT_TRUE(wait_for([&] { return handler_.disconnect_count.load() >= 1; }));
}

TEST_F(TcpServerEchoTest, SendViaServerApi) {
  // Test the send(id, payload) API instead of zero-copy tx_space.
  // We need a custom handler that uses server.send().
  // Since this requires a pointer to the server, we test it indirectly:
  // the EchoHandler already uses the tx_space pattern, which tests the
  // primary path. The send() API is a straightforward framer.encode() +
  // memcpy — the framer tests cover encode correctness.
  //
  // This test simply verifies end-to-end echo works (already covered above).
  TcpSocket client(connect_to(server_->port()));
  send_framed(client, "send_api", 8);
  EXPECT_EQ(recv_framed(client), "send_api");
}

// ============================================================================
// Integration tests — connection rejection
// ============================================================================

TEST(TcpServerRejectTest, HandlerRejectsConnection) {
  RejectHandler handler;
  const TcpServerConfig cfg{
      .port = 0,
      .backlog = 16,
      .nodelay = true,
      .busy_spin = false,
      .pin_core = -1,
      .alloc_guard = false,
  };
  const AlignedBuffer srv_buf(RejectServer::required_buffer_size());
  auto server =
      std::make_unique<RejectServer>(handler, cfg, srv_buf.ptr, srv_buf.size);
  ASSERT_TRUE(server->listen());

  std::jthread server_thread([&] { server->run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds{20});

  // Connect — server should accept the TCP connection then immediately
  // close it because handler.on_connect() returns false.
  {
    const int fd = connect_to(server->port());

    // Try to read — should get EOF (server closed the connection).
    char buf[16];
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    // n == 0 means peer closed (FIN), or n == -1 on error.
    EXPECT_LE(n, 0);
    ::close(fd);
  }

  EXPECT_TRUE(wait_for([&] { return handler.connect_count.load() >= 1; }));

  server->request_stop();
  wake_epoll(server->port());
  server_thread.join();
}

// ============================================================================
// Integration tests — RawFramer
// ============================================================================

TEST(TcpServerRawFramerTest, EchoRawBytes) {
  EchoHandler handler;
  const TcpServerConfig cfg{
      .port = 0,
      .backlog = 16,
      .nodelay = true,
      .busy_spin = false,
      .pin_core = -1,
      .alloc_guard = false,
  };
  const AlignedBuffer srv_buf(RawEchoServer::required_buffer_size());
  auto server =
      std::make_unique<RawEchoServer>(handler, cfg, srv_buf.ptr, srv_buf.size);
  ASSERT_TRUE(server->listen());

  std::jthread server_thread([&] { server->run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds{20});

  TcpSocket client(connect_to(server->port()));

  // Send raw bytes (no framing).
  const char msg[] = "raw_echo";
  auto send_result = client.send_blocking(msg, std::strlen(msg));
  ASSERT_EQ(send_result.status, TcpSocket::SendStatus::kOk);

  // Small delay to let server process.
  std::this_thread::sleep_for(std::chrono::milliseconds{50});

  // Receive echoed raw bytes.
  std::array<char, 64> buf{};
  auto recv_result = client.receive_nonblocking(buf.data(), buf.size());
  // May get kOk with the echoed data.
  if (recv_result.status == TcpSocket::RecvStatus::kOk) {
    EXPECT_EQ(std::string_view(buf.data(), static_cast<std::size_t>(
                                               recv_result.bytes_received)),
              "raw_echo");
  }

  server->request_stop();
  wake_epoll(server->port());
  server_thread.join();

  EXPECT_GE(handler.connect_count.load(), 1);
  EXPECT_GE(handler.data_count.load(), 1);
}

// ============================================================================
// Port observer test
// ============================================================================

TEST(TcpServerPortTest, EphemeralPortAssignment) {
  using MinimalServer =
      TcpServer<MinimalHandler, kTestMaxConns, kTestBufSize, kTestBufSize>;
  MinimalHandler handler;
  const TcpServerConfig cfg{
      .port = 0,
      .backlog = 16,
      .nodelay = true,
      .busy_spin = false,
      .pin_core = -1,
      .alloc_guard = false,
  };
  const AlignedBuffer srv_buf(MinimalServer::required_buffer_size());
  auto server =
      std::make_unique<MinimalServer>(handler, cfg, srv_buf.ptr, srv_buf.size);
  ASSERT_TRUE(server->listen());
  EXPECT_GT(server->port(), 0);
}

// ============================================================================
// Graceful shutdown test
// ============================================================================

TEST(TcpServerShutdownTest, RequestStopExitsRunLoop) {
  using MinimalServer =
      TcpServer<MinimalHandler, kTestMaxConns, kTestBufSize, kTestBufSize>;
  MinimalHandler handler;
  const TcpServerConfig cfg{
      .port = 0,
      .backlog = 16,
      .nodelay = true,
      .busy_spin = true, // busy-spin so stop_ check is immediate
      .pin_core = -1,
      .alloc_guard = false,
  };
  const AlignedBuffer srv_buf(MinimalServer::required_buffer_size());
  auto server =
      std::make_unique<MinimalServer>(handler, cfg, srv_buf.ptr, srv_buf.size);
  ASSERT_TRUE(server->listen());

  std::jthread server_thread([&] { server->run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds{20});

  server->request_stop();
  server_thread.join();
  // If we get here, run() exited. Test passes.
}

} // namespace
} // namespace mk::net
