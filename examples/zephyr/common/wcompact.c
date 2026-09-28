/* WEngine compact v2 decoder implementation. See wcompact.h. */

#include "wcompact.h"

static uint32_t rd_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64(const uint8_t *p) {
  uint64_t v = 0;
  size_t k;
  for (k = 0; k < 8; k++) {
    v |= (uint64_t)p[k] << (8 * k);
  }
  return v;
}

/* Little-endian IEEE-754 bits to double without aliasing. */
static double rd_f64(const uint8_t *p) {
  uint64_t bits = rd_u64(p);
  double v = 0.0;
  uint8_t *dst = (uint8_t *)&v;
  size_t k;
  for (k = 0; k < 8; k++) {
    dst[k] = (uint8_t)(bits >> (8 * k));
  }
  return v;
}

int wcompact_decode(const uint8_t *buf, size_t len, wcompact_frame_t *out) {
  uint32_t n_items;
  uint32_t n_ch;
  size_t at;
  uint32_t k;
  size_t c;
  wcompact_frame_t f;
  if (buf == 0 || out == 0) {
    return -1;
  }
  if (len < (size_t)WCOMPACT_HEADER) {
    return -1;
  }
  if (rd_u32(buf) != 0x32564C57u || rd_u32(buf + 4) != 2u || buf[8] != 1u) {
    return -2;
  }
  if (buf[9] != 0 || buf[10] != 0 || buf[11] != 0) {
    return -3;
  }
  /* Domain: NUL-terminated within 32 bytes, non-empty. */
  {
    size_t z = 0;
    while (z < 32 && buf[44 + z] != 0) {
      z++;
    }
    if (z == 0 || z >= 32) {
      return -3;
    }
    for (c = 0; c < z; c++) {
      f.domain[c] = (char)buf[44 + c];
    }
    f.domain[z] = '\0';
  }
  n_items = rd_u32(buf + 76);
  n_ch = rd_u32(buf + 80);
  if (n_items > 1024u || n_ch > (uint32_t)WCOMPACT_MAX_CHANNELS) {
    return -3;
  }
  f.run_id = rd_u64(buf + 12);
  f.frame_seq = rd_u64(buf + 20);
  f.step_index = rd_u64(buf + 28);
  f.sim_time = rd_f64(buf + 36);
  f.n_channels = n_ch;
  /* Exact-length walk: items, then every channel, ending exactly at len. */
  at = (size_t)WCOMPACT_HEADER;
  for (k = 0; k < n_items; k++) {
    if (len - at < (size_t)WCOMPACT_ITEM) {
      return -1;
    }
    at += (size_t)WCOMPACT_ITEM;
  }
  for (k = 0; k < n_ch; k++) {
    uint32_t nl;
    size_t ci;
    if (at >= len) {
      return -1;
    }
    nl = buf[at++];
    if (nl > (uint32_t)WCOMPACT_NAME_MAX) {
      return -4; // name cannot exist: corrupt length, not truncation
    }
    if (len - at < (size_t)nl + 8u) {
      return -1; // channel bytes missing: truncated record
    }
    for (ci = 0; ci < (size_t)nl; ci++) {
      f.ch[k].name[ci] = (char)buf[at + ci];
    }
    f.ch[k].name[nl] = '\0';
    f.ch[k].value = rd_f64(buf + at + nl);
    at += (size_t)nl + 8u;
  }
  if (at != len) {
    return -4;
  }
  *out = f;
  return 0;
}
