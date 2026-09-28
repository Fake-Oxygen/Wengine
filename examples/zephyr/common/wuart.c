/* WEngine UART parser implementation. See wuart.h. Style: C99, no libc
 * calls, all state in the caller-owned struct (no statics: the host test
 * runs two demuxers side by side). */

#include "wuart.h"

static int is_digit(char c) {
  return c >= '0' && c <= '9';
}

/* Strict u64 (no signs, no empty, overflow-checked). */
static int parse_u64(const char *s, size_t n, uint64_t *out) {
  uint64_t acc = 0;
  size_t i;
  if (n == 0) {
    return -1;
  }
  for (i = 0; i < n; i++) {
    unsigned d;
    if (!is_digit(s[i])) {
      return -1;
    }
    d = (unsigned)(s[i] - '0');
    if (acc > (0xFFFFFFFFFFFFFFFFull - d) / 10u) {
      return -1;
    }
    acc = acc * 10u + d;
  }
  *out = acc;
  return 0;
}

/* Strict decimal double. Grammar: [+-]? digits [. digits] [(e|E) [+-]?
 * digits], at least one mantissa digit. No inf/nan/hex (%.17g never
 * emits them). Value via Horner + binary exponent scaling — exact for
 * the short decimals %.17g emits, and the host test asserts bit equality
 * against strtod on the golden corpus. */
static int parse_f64(const char *s, size_t n, double *out) {
  int neg = 0;
  double intpart = 0.0;
  double fracpart = 0.0;
  double fracscale = 1.0;
  int expval = 0;
  int expneg = 0;
  int digits = 0;
  int expdigits = 0;
  size_t i = 0;
  double v;
  int e;
  if (n == 0) {
    return -1;
  }
  if (s[i] == '+' || s[i] == '-') {
    neg = (s[i] == '-');
    if (++i >= n) {
      return -1;
    }
  }
  while (i < n && is_digit(s[i])) {
    intpart = intpart * 10.0 + (double)(s[i] - '0');
    digits = 1;
    i++;
  }
  if (i < n && s[i] == '.') {
    i++;
    while (i < n && is_digit(s[i])) {
      fracpart = fracpart * 10.0 + (double)(s[i] - '0');
      fracscale *= 10.0;
      digits = 1;
      i++;
    }
  }
  if (!digits) {
    return -1;
  }
  if (i < n && (s[i] == 'e' || s[i] == 'E')) {
    if (++i >= n) {
      return -1;
    }
    if (s[i] == '+' || s[i] == '-') {
      expneg = (s[i] == '-');
      if (++i >= n) {
        return -1;
      }
    }
    while (i < n && is_digit(s[i])) {
      expval = expval * 10 + (s[i] - '0');
      if (expval > 1000000) {
        return -1;
      }
      expdigits = 1;
      i++;
    }
    if (!expdigits) {
      return -1;
    }
  }
  if (i != n) {
    return -1;
  }
  v = intpart + fracpart / fracscale;
  e = expneg ? -expval : expval;
  while (e >= 10) {
    v *= 1e10;
    e -= 10;
  }
  while (e >= 1) {
    v *= 10.0;
    e -= 1;
  }
  while (e <= -10) {
    v /= 1e10;
    e += 10;
  }
  while (e <= -1) {
    v /= 10.0;
    e += 1;
  }
  *out = neg ? -v : v;
  return 0;
}

int wuart_parse_sample(const char *line, wuart_sample_t *out) {
  size_t fields[6];
  size_t nf = 0;
  size_t i = 0;
  wuart_sample_t s;
  if (line == 0 || out == 0) {
    return -1;
  }
  fields[0] = 0;
  for (i = 0; line[i] != '\0'; i++) {
    if (line[i] == ',') {
      nf++;
      if (nf > 4) {
        return -1;
      }
      fields[nf] = i + 1;
    }
  }
  if (nf != 4) {
    return -1;
  }
  fields[5] = i; /* end of last field */
  if (parse_u64(line + fields[0], fields[1] - fields[0] - 1, &s.step) != 0) {
    return -1;
  }
  if (parse_f64(line + fields[1], fields[2] - fields[1] - 1, &s.time) != 0) {
    return -1;
  }
  if (parse_u64(line + fields[2], fields[3] - fields[2] - 1, &s.sig_id) != 0) {
    return -1;
  }
  if (parse_f64(line + fields[3], fields[4] - fields[3] - 1, &s.value) != 0) {
    return -1;
  }
  {
    size_t qlen = fields[5] - fields[4];
    if (qlen != 1 || (line[fields[4]] != 'U' && line[fields[4]] != 'I')) {
      return -1;
    }
    s.qty = line[fields[4]];
  }
  *out = s;
  return 0;
}

void wuart_init(wuart_demux_t *d, int cobs) {
  size_t i;
  d->cobs = cobs ? 1 : 0;
  d->len = 0;
  d->text_len = 0;
  d->text[0] = '\0';
  for (i = 0; i < sizeof(d->raw); i++) {
    d->raw[i] = 0;
  }
}

/* COBS-encode a NUL-terminated payload (see header). */
size_t wuart_cobs_encode(const char *payload, char *out, size_t out_cap) {
  size_t n = 0;
  size_t code_at = 0;
  unsigned run = 1;
  size_t i = 0;
  size_t o;
  if (payload == 0 || out == 0 || out_cap < 2) {
    return 0;
  }
  while (payload[n] != '\0') {
    n++;
  }
  if (n == 0 || n + n / 254u + 2u > out_cap) {
    return 0;
  }
  out[0] = 1; /* placeholder code byte */
  o = 1;
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)payload[i];
    if (c == 0) {
      out[code_at] = (char)run;
      code_at = o;
      out[o++] = 1;
      run = 1;
    } else {
      out[o++] = (char)c;
      if (++run == 255) {
        out[code_at] = (char)255;
        code_at = o;
        out[o++] = 1;
        run = 1;
      }
    }
  }
  out[code_at] = (char)run;
  return o;
}

/* COBS-decode raw[0..n) into d->text. Returns 0 with text_len set, or -1
 * (empty frame, zero code byte, span overrun). */
static int demux_cobs_decode(wuart_demux_t *d, size_t n) {
  size_t i = 0;
  size_t o = 0;
  if (n == 0) {
    return -1;
  }
  while (i < n) {
    unsigned code = d->raw[i++];
    size_t span;
    size_t k;
    if (code == 0) {
      return -1;
    }
    span = (size_t)(code - 1);
    if (i + span > n || o + span + 1 >= (size_t)WUART_LINE_MAX) {
      return -1;
    }
    for (k = 0; k < span; k++) {
      d->text[o++] = (char)d->raw[i++];
    }
    if (code < 255 && i < n) {
      d->text[o++] = '\0'; /* implicit zero between blocks */
    }
  }
  d->text[o] = '\0';
  d->text_len = o;
  return 0;
}

/* Copy an ascii unit (no delimiter) into text. Returns 0, or -1 when the
 * unit exceeds the line buffer. */
static int demux_copy_text(wuart_demux_t *d, size_t n) {
  size_t k;
  if (n + 1 >= (size_t)WUART_LINE_MAX) {
    return -1;
  }
  for (k = 0; k < n; k++) {
    d->text[k] = (char)d->raw[k];
  }
  d->text[n] = '\0';
  d->text_len = n;
  return 0;
}

wuart_event_t wuart_feed(wuart_demux_t *d, uint8_t byte) {
  if (!d->cobs) {
    /* ASCII: units end at '\n' (stray '\r' stripped); '!' marks control. */
    if (byte == '\n') {
      size_t n = d->len;
      d->len = 0;
      if (n > 0 && d->raw[n - 1] == (uint8_t)'\r') {
        n--;
      }
      if (n == 0) {
        return WUART_NEED_MORE; /* blank line: line noise, ignore */
      }
      if (demux_copy_text(d, n) != 0) {
        return WUART_MALFORMED;
      }
      if (d->text[0] == '!') {
        size_t k;
        if (d->text_len < 2) {
          return WUART_MALFORMED; /* bare '!' carries no command */
        }
        for (k = 0; k + 1 < d->text_len; k++) {
          d->text[k] = d->text[k + 1];
        }
        d->text_len--;
        d->text[d->text_len] = '\0';
        /* Always CMD: the demux is side-agnostic. Device apps only ever
         * receive responses here (OK/ERR) and host tests only ever feed
         * commands — each side classifies by its own role. */
        return WUART_CMD;
      }
      return WUART_STREAM;
    }
    if (d->len >= sizeof(d->raw)) {
      d->len = 0;
      return WUART_MALFORMED; /* delimiter-free run past the cap */
    }
    d->raw[d->len++] = byte;
    return WUART_NEED_MORE;
  }
  /* COBS: units end at 0x00; type byte selects the event. */
  if (byte == 0x00) {
    size_t n = d->len;
    char type;
    d->len = 0;
    if (n == 0) {
      return WUART_NEED_MORE; /* stray delimiter: ignore */
    }
    if (demux_cobs_decode(d, n) != 0 || d->text_len < 1) {
      return WUART_MALFORMED;
    }
    type = d->text[0];
    {
      /* Strip the type byte in place. */
      size_t k;
      for (k = 0; k + 1 < d->text_len; k++) {
        d->text[k] = d->text[k + 1];
      }
      d->text_len--;
      d->text[d->text_len] = '\0';
    }
    if (type == 'S') {
      return WUART_STREAM;
    }
    if (type == 'Q') {
      return d->text_len > 0 ? WUART_CMD : WUART_MALFORMED;
    }
    if (type == 'R') {
      return d->text_len > 0 ? WUART_RSP : WUART_MALFORMED;
    }
    return WUART_MALFORMED;
  }
  if (d->len >= sizeof(d->raw)) {
    d->len = 0;
    return WUART_MALFORMED;
  }
  d->raw[d->len++] = byte;
  return WUART_NEED_MORE;
}
