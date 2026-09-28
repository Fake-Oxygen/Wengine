/* WEngine compact v2 record decoder (Zephyr socket sample, SIG-8).
 *
 * C99, no libc beyond <stdint.h>/<stddef.h>. Decodes exactly what
 * link/compact_codec.hpp documents (magic "WLV2", schema 2, kind 1,
 * 84 B header, 61 B items skipped by count, name_len+name+f64 channels)
 * and refuses everything else loudly: -1 truncated, -2 wrong
 * magic/schema/kind, -3 bad reserved/counts/domain, -4 corrupt name
 * length or trailing bytes. Shared with the host unit test, which feeds
 * it real TcpPublisher bytes.
 */
#ifndef WCOMPACT_H
#define WCOMPACT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WCOMPACT_MAX_CHANNELS 64u
#define WCOMPACT_NAME_MAX 31u
#define WCOMPACT_HEADER 84u
#define WCOMPACT_ITEM 61u

typedef struct {
  char name[WCOMPACT_NAME_MAX + 1u];
  double value;
} wcompact_channel_t;

typedef struct {
  uint64_t run_id;
  uint64_t frame_seq;
  uint64_t step_index;
  double sim_time;
  char domain[33];
  uint32_t n_channels;
  wcompact_channel_t ch[WCOMPACT_MAX_CHANNELS];
} wcompact_frame_t;

int wcompact_decode(const uint8_t *buf, size_t len, wcompact_frame_t *out);

#ifdef __cplusplus
}
#endif

#endif /* WCOMPACT_H */
