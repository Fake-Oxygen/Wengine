/* WEngine UART line demux + sample parser (Zephyr sample, SIG-8).
 *
 * C99, no libc beyond <stdint.h>/<stddef.h> (the doubles use a small
 * hand-rolled decimal parser, so results never depend on locale or on the
 * board's printf/scanf configuration). Shared with the host unit test,
 * which compiles this exact file and cross-checks it against the C++
 * builder (link/uart_codec.hpp) on a golden corpus.
 *
 * Wire grammar (docs/zephyr-hil.md):
 *   ascii: "<step>,<time>,<sig_id>,<value>,<quantity>\n"   stream
 *          "!<control line>\n"                             control, both ways
 *   cobs:  COBS('S' + line) 0x00   stream
 *          COBS('Q' + line) 0x00   device->host command
 *          COBS('R' + line) 0x00   host->device response
 *
 * Feed bytes one at a time; each call returns at most one event. STREAM /
 * CMD / RSP events expose the line (NUL-terminated) in ev_text/ev_len.
 * MALFORMED covers one bad unit (bad field, bad COBS, wrong type byte,
 * over-long unit): count it and carry on — the next delimiter resyncs.
 * NEED_MORE means keep feeding. Buffer limits are protocol-true
 * (4096 B control lines); the struct is ~8 KB static.
 */
#ifndef WUART_H
#define WUART_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WUART_LINE_MAX 4224u

typedef enum {
  WUART_NEED_MORE = 0,
  WUART_STREAM = 1,
  WUART_CMD = 2,
  WUART_RSP = 3,
  WUART_MALFORMED = 4
} wuart_event_t;

typedef struct {
  uint64_t step;
  double time;
  uint64_t sig_id;
  double value;
  char qty; /* 'U' or 'I' */
} wuart_sample_t;

/* Strict 5-field sample parse. Returns 0 on success, -1 on any shape,
 * numeric, or quantity violation (line untouched on failure). */
int wuart_parse_sample(const char *line, wuart_sample_t *out);

typedef struct {
  int cobs; /* 0 = ascii lines, nonzero = COBS frames */
  uint8_t raw[WUART_LINE_MAX + 64u];
  size_t len;
  char text[WUART_LINE_MAX];
  size_t text_len;
} wuart_demux_t;

void wuart_init(wuart_demux_t *d, int cobs);
wuart_event_t wuart_feed(wuart_demux_t *d, uint8_t byte);

/* COBS-encode a NUL-terminated payload (already including its 'S'/'Q'/'R'
 * type byte for frames) without the 0x00 delimiter. Returns the encoded
 * length, or 0 when out_cap is too small (payloads here are short lines,
 * so size callers for payload + payload/254 + 3). */
size_t wuart_cobs_encode(const char *payload, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* WUART_H */
