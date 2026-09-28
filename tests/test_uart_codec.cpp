// SIG-8 UART wire codec tests: COBS vectors, endpoint/mode parsing,
// stream line goldens, ascii/cobs demux, and the shared Zephyr C parsers
// (compiled here unchanged) cross-checked against the C++ builders —
// including the compact v2 record decoder fed with real encoder bytes.

#include "wengine/link/compact_codec.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/uart_codec.hpp"
#include "wengine/world/state.hpp"

#include "wcompact.h"
#include "wuart.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace wengine;

namespace {

constexpr bool near(double a, double b, double eps = 1e-12) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

void test_cobs_vectors() {
  using link::uart::cobs_decode;
  using link::uart::cobs_encode;
  auto enc = [](std::string_view in) {
    std::string out;
    assert(cobs_encode(in, out).ok());
    return out;
  };
  // Empty input encodes to one code byte; empty frames never decode.
  assert(enc("") == std::string("\x01", 1));
  assert(!cobs_decode("").ok());
  // Zero-free run: single code byte + data.
  assert(enc("hello") == std::string("\x06hello", 6));
  {
    const auto d = cobs_decode("\x06hello");
    assert(d.ok() && d.value == "hello");
  }
  // Embedded zeros split blocks with implicit-zero reinsertion.
  {
    const std::string in("a\x00"
                         "b",
                         3);
    const std::string e = enc(in);
    assert(e == (std::string("\x02", 1) + "a" + std::string("\x02", 1) + "b"));
    const auto d = cobs_decode(e);
    assert(d.ok() && d.value == in);
  }
  // 254 non-zero bytes: code 255 fills exactly, plus the final 0x01.
  {
    const std::string in(254, 'x');
    const std::string e = enc(in);
    assert(e.size() == 256 && static_cast<unsigned char>(e[0]) == 255 &&
           static_cast<unsigned char>(e[255]) == 1);
    const auto d = cobs_decode(e);
    assert(d.ok() && d.value == in);
  }
  // 255 non-zero bytes: split 255 + short block.
  {
    const std::string in(255, 'y');
    const std::string e = enc(in);
    assert(e.size() == 257 && static_cast<unsigned char>(e[0]) == 255 &&
           static_cast<unsigned char>(e[255]) == 2);
    const auto d = cobs_decode(e);
    assert(d.ok() && d.value == in);
  }
  // All zeros: one 2-code per byte.
  {
    const std::string in("\x00\x00\x00", 3);
    const auto d = cobs_decode(enc(in));
    assert(d.ok() && d.value == in);
  }
  // Refusals: embedded 0x00, span overrun, truncated tail.
  assert(!cobs_decode(std::string("\x02a\x00", 3)).ok());
  assert(!cobs_decode(std::string("\x05", 1) + "ab").ok());
  assert(!cobs_decode("\xFF").ok());
  // Long round-trip incl. zeros every 7 bytes (overhead ~ one code
  // byte per zero plus block codes — decode equality is the assertion).
  {
    std::string in;
    for (int k = 0; k < 600; ++k) {
      in.push_back(k % 7 == 0 ? '\0' : static_cast<char>('A' + (k % 26)));
    }
    const std::string e = enc(in);
    assert(e.size() <= in.size() + 100);
    const auto d = cobs_decode(e);
    assert(d.ok() && d.value == in);
  }
}

void test_c_encoder_parity() {
  // The C encoder in the Zephyr sample must emit byte-identical frames.
  const char *corpus[] = {"S1,2,3,4,U", "QSET 0 AMP 2", "R", "S", "hello world"};
  for (const char *p : corpus) {
    std::string want;
    assert(link::uart::cobs_encode(p, want).ok());
    char got[160];
    const size_t n = wuart_cobs_encode(p, got, sizeof(got));
    assert(n == want.size());
    assert(std::string(got, n) == want);
  }
  // Empty payload and too-small output refuse loudly (never truncate).
  {
    char tiny[3];
    assert(wuart_cobs_encode("", tiny, sizeof(tiny)) == 0);
    assert(wuart_cobs_encode("hello world, this is long", tiny, sizeof(tiny)) == 0);
  }
}

void test_endpoint_and_mode() {
  using link::uart::parse_mode;
  using link::uart::parse_uart_endpoint;
  using link::uart::UartMode;
  {
    const auto r = parse_uart_endpoint("/dev/ttyUSB0");
    assert(r.ok() && r.value.device == "/dev/ttyUSB0" &&
           r.value.baud == link::uart::kDefaultUartBaud);
  }
  {
    const auto r = parse_uart_endpoint("/dev/ttyUSB0,9600");
    assert(r.ok() && r.value.baud == 9600u);
  }
  {
    const auto r = parse_uart_endpoint("/dev/pts/6,921600");
    assert(r.ok() && r.value.baud == 921600u);
  }
  assert(!parse_uart_endpoint("").ok());
  assert(!parse_uart_endpoint("/dev/x,").ok());
  assert(!parse_uart_endpoint(",115200").ok());
  assert(!parse_uart_endpoint("/dev/x,abc").ok());
  assert(!parse_uart_endpoint("/dev/x,12345").ok()); // unsupported rate
  assert(!parse_uart_endpoint("/dev/x,99999999").ok());
  assert(parse_mode("ascii").ok() && parse_mode("ascii").value == UartMode::kAscii);
  assert(parse_mode("cobs").ok() && parse_mode("cobs").value == UartMode::kCobs);
  assert(!parse_mode("binary").ok() && !parse_mode("").ok());
}

void test_line_goldens_and_c_parser() {
  using link::uart::build_stream_line;
  using link::uart::parse_stream_line;
  using link::uart::StreamSample;
  // Golden corpus: builder output parses back exactly (C++ and C agree).
  struct Case {
    std::uint64_t step;
    double time;
    std::uint64_t id;
    double value;
    const char *qty;
  };
  const Case cases[] = {
      {1, 0.0001, 0, 1.9396913325651612, "U"},
      {1000000, 123.456, 42, -2.5e-3, "I"},
      {7, 0.0, 3, 0.0, "U"},
      {999, 1.0, 0, 3.141592653589793, "I"},
  };
  for (const Case &c : cases) {
    const std::string line =
        build_stream_line(c.step, c.time, c.id, c.value, c.qty);
    StreamSample s;
    assert(parse_stream_line(line, s).ok());
    assert(s.step == c.step && s.sig_id == c.id && s.time == c.time &&
           s.value == c.value && s.quantity == c.qty);
    wuart_sample_t cs;
    assert(wuart_parse_sample(line.c_str(), &cs) == 0);
    // Integer fields and quantity are exact; doubles use a justified
    // epsilon (Horner evaluation can differ from the literal by an ulp).
    assert(cs.step == c.step && cs.sig_id == c.id && cs.qty == c.qty[0]);
    assert(near(cs.time, c.time) && near(cs.value, c.value));
  }
  // Malformed lines: field counts, numerics, quantity.
  const char *bad[] = {"", "1,2,3", "1,2,3,4,U,X", "x,0,0,0,U", "1,x,0,0,U",
                       "1,0,x,0,U", "1,0,0,x,U",  "1,0,0,0,X", "1,0,0,0,",
                       "1,0,0,inf,U", "1,,0,0,U"};
  for (const char *b : bad) {
    StreamSample s;
    assert(!parse_stream_line(b, s).ok());
    wuart_sample_t cs;
    assert(wuart_parse_sample(b, &cs) != 0);
  }
}

void feed_all(wuart_demux_t *d, const std::string &bytes,
              std::vector<std::pair<wuart_event_t, std::string>> &events) {
  for (unsigned char c : bytes) {
    const wuart_event_t ev = wuart_feed(d, c);
    if (ev != WUART_NEED_MORE) {
      events.emplace_back(ev, std::string(d->text, d->text_len));
    }
  }
}

void test_ascii_demux() {
  wuart_demux_t d;
  wuart_init(&d, 0);
  std::vector<std::pair<wuart_event_t, std::string>> ev;
  feed_all(&d, "1,0.1,0,2,U\n", ev);
  feed_all(&d, "!PING\n", ev);
  feed_all(&d, "\n", ev);            // blank: ignored silently
  feed_all(&d, "GARBAGE!\n", ev);    // no prefix, no commas: still a unit
  feed_all(&d, "!\n", ev);           // bare marker: malformed
  feed_all(&d, "!SET 0 AMP 2\r\n", ev); // CR stripped
  assert(ev.size() == 5);
  assert(ev[0].first == WUART_STREAM && ev[0].second == "1,0.1,0,2,U");
  assert(ev[1].first == WUART_CMD && ev[1].second == "PING");
  assert(ev[2].first == WUART_STREAM && ev[2].second == "GARBAGE!");
  assert(ev[3].first == WUART_MALFORMED);
  assert(ev[4].first == WUART_CMD && ev[4].second == "SET 0 AMP 2");
  // The stream unit above parses; the garbage one does not (reader counts).
  wuart_sample_t s;
  assert(wuart_parse_sample(ev[0].second.c_str(), &s) == 0);
  assert(wuart_parse_sample(ev[2].second.c_str(), &s) != 0);
}

void test_cobs_demux_and_resync() {
  using link::uart::cobs_encode;
  wuart_demux_t d;
  wuart_init(&d, 1);
  auto frame = [](const std::string &payload) {
    std::string f;
    assert(cobs_encode(payload, f).ok());
    f.push_back('\0');
    return f;
  };
  std::vector<std::pair<wuart_event_t, std::string>> ev;
  feed_all(&d, frame("S1,0.1,0,2,U"), ev);
  feed_all(&d, frame("QPING"), ev);
  feed_all(&d, frame("ROK pong"), ev);
  feed_all(&d, frame("Xbogus"), ev); // unknown type: malformed
  feed_all(&d, std::string("\x00", 1), ev); // stray delimiter: ignored
  feed_all(&d, std::string("zz\xFF"
                           "z",
                           4) +
                   std::string("\x00", 1),
           ev); // bad COBS then resync...
  feed_all(&d, frame("S2,0.2,0,3,U"), ev); // ...next frame decodes cleanly
  assert(ev.size() == 6);
  assert(ev[0].first == WUART_STREAM && ev[0].second == "1,0.1,0,2,U");
  assert(ev[1].first == WUART_CMD && ev[1].second == "PING");
  assert(ev[2].first == WUART_RSP && ev[2].second == "OK pong");
  assert(ev[3].first == WUART_MALFORMED);
  assert(ev[4].first == WUART_MALFORMED);
  assert(ev[5].first == WUART_STREAM && ev[5].second == "2,0.2,0,3,U");
}

void test_compact_c_decoder_against_encoder() {
  // Real encoder bytes in, C decoder out: step/time/channels exact.
  world::Snapshot snap;
  snap.sim_time = 0.5;
  snap.step_index = 7;
  snap.sig_serial = {5};
  snap.sig_value = {1.25};
  snap.sig_quantity = {static_cast<std::uint8_t>(signal::QuantityId::kVoltage)};
  const link::Frame frame = link::frame_from_signal_snapshot(snap);
  std::vector<unsigned char> buf(link::compact::kMaxRecordBytes);
  std::size_t len = 0;
  assert(link::compact::encode_record(frame, "signal", 99, 12, buf.data(), buf.size(), len)
             .ok());
  wcompact_frame_t got;
  assert(wcompact_decode(buf.data(), len, &got) == 0);
  assert(got.run_id == 99 && got.frame_seq == 12 && got.step_index == 7);
  assert(got.sim_time == 0.5 && std::string(got.domain) == "signal");
  assert(got.n_channels == 2); // sig.count + sig.5.U
  assert(std::string(got.ch[0].name) == "sig.count" && got.ch[0].value == 1.0);
  assert(std::string(got.ch[1].name) == "sig.5.U" && got.ch[1].value == 1.25);
  // Refusals: truncated, bad magic, bad counts, trailing byte.
  assert(wcompact_decode(buf.data(), len - 1, &got) == -1);
  {
    std::vector<unsigned char> bad = buf;
    bad[0] ^= 0xFF;
    assert(wcompact_decode(bad.data(), len, &got) == -2);
  }
  {
    std::vector<unsigned char> bad(buf.begin(), buf.begin() + len);
    bad[80] = 65; // channel_count 65 > 64
    assert(wcompact_decode(bad.data(), len, &got) == -3);
  }
  {
    std::vector<unsigned char> bad(buf.begin(), buf.begin() + len);
    bad.push_back(0);
    assert(wcompact_decode(bad.data(), bad.size(), &got) == -4);
  }
}

} // namespace

int main() {
  test_cobs_vectors();
  test_c_encoder_parity();
  test_endpoint_and_mode();
  test_line_goldens_and_c_parser();
  test_ascii_demux();
  test_cobs_demux_and_resync();
  test_compact_c_decoder_against_encoder();
  std::cout << "test_uart_codec: OK\n";
  return 0;
}
