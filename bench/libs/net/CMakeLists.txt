mk_add_bench(wire_codec_bench               wire_codec_bench.cpp               mk_net)
mk_add_bench(packed_codec_bench             packed_codec_bench.cpp             mk_net)
mk_add_bench(message_codec_bench            message_codec_bench.cpp            mk_net)
mk_add_bench(length_prefix_codec_bench      length_prefix_codec_bench.cpp      mk_net)

mk_add_gbench(wire_codec_gbench               wire_codec_gbench.cpp               mk_net)
mk_add_gbench(packed_codec_gbench             packed_codec_gbench.cpp             mk_net)
mk_add_gbench(message_codec_gbench            message_codec_gbench.cpp            mk_net)
mk_add_gbench(length_prefix_codec_gbench      length_prefix_codec_gbench.cpp      mk_net)
