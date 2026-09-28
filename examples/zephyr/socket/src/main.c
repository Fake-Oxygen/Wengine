/* WEngine TCP socket sample (SIG-8): framing-level Zephyr reader.
 *
 * Connects to `sig run --emit-tcp host:port`, validates the WLT2
 * hello/version/domain gate exactly like the Python receivers, then
 * decodes every compact v2 record with the shared C decoder
 * (../common/wcompact.c) and prints step/sequence/channels. Loss is
 * reported from frame_seq gaps (same LagStats vocabulary as the host).
 *
 * Sample limits (documented, not silent): records larger than 8 KiB are
 * refused (a 63-source signal record is ~1 KiB); only kind-1 frames
 * decode; sample-block records (PHYSI-44) report their kind and are
 * skipped by length. Zephyr-side IP setup (interface/DHCP) is board
 * specific — see ../README.md.
 *
 * Host side:
 *   ./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1kHz \
 *     --sig-amp 3.3V --steps 1000 --dt 0.1ms --emit-tcp 0.0.0.0:9000
 */

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/printk.h>

#include <stdio.h>
#include <string.h>

#include "wcompact.h"

#define HELLO_LEN 40u
#define MAX_RECORD 8192u

static int recv_all(int fd, uint8_t *buf, size_t len) {
  size_t got = 0;
  while (got < len) {
    int n = zsock_recv(fd, buf + got, len - got, 0);
    if (n <= 0) {
      return -1;
    }
    got += (size_t)n;
  }
  return 0;
}

static uint32_t rd_u32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

int main(void) {
  static uint8_t hello[HELLO_LEN];
  static uint8_t record[MAX_RECORD];
  uint64_t last_seq = 0;
  uint64_t frames = 0;
  uint64_t missed = 0;
  uint64_t refused = 0;
  int first = 1;
  int fd;
  struct zsock_addrinfo *ai = NULL;
  struct zsock_addrinfo hints;
  char port[8];

  printk("wengine: socket sample -> %s:%d\n", CONFIG_WENGINE_HOST,
         CONFIG_WENGINE_PORT);
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(port, sizeof(port), "%d", CONFIG_WENGINE_PORT);
  if (zsock_getaddrinfo(CONFIG_WENGINE_HOST, port, &hints, &ai) != 0 ||
      ai == NULL) {
    printk("wengine: DNS/resolve failed\n");
    return 0;
  }
  fd = zsock_socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (fd < 0) {
    printk("wengine: socket failed\n");
    zsock_freeaddrinfo(ai);
    return 0;
  }
  if (zsock_connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
    printk("wengine: connect failed\n");
    zsock_close(fd);
    zsock_freeaddrinfo(ai);
    return 0;
  }
  zsock_freeaddrinfo(ai);
  /* Hello gate: magic "WLT2" + exact schema + NUL-terminated,
   * non-empty domain (same wording as the C++/Python readers). */
  if (recv_all(fd, hello, sizeof(hello)) != 0 || rd_u32le(hello) != 0x32544C57u ||
      rd_u32le(hello + 4) != 2u) {
    printk("wengine: hello refused (magic/schema)\n");
    zsock_close(fd);
    return 0;
  }
  {
    size_t z = 0;
    while (z < 32 && hello[8 + z] != 0) {
      z++;
    }
    if (z == 0 || z >= 32) {
      printk("wengine: hello refused (domain)\n");
      zsock_close(fd);
      return 0;
    }
    printk("wengine: hello ok, domain '%s'\n", (const char *)(hello + 8));
  }
  for (;;) {
    uint8_t lenbuf[4];
    uint32_t reclen;
    wcompact_frame_t fr;
    uint32_t c;
    int derr;
    if (recv_all(fd, lenbuf, sizeof(lenbuf)) != 0) {
      printk("wengine: stream closed (frames=%llu missed=%llu refused=%llu)\n",
             frames, missed, refused);
      break;
    }
    reclen = rd_u32le(lenbuf);
    if (reclen == 0 || reclen > (uint32_t)MAX_RECORD) {
      printk("wengine: over-bound record (%u B) refused\n", reclen);
      refused++;
      zsock_close(fd);
      return 0;
    }
    if (recv_all(fd, record, reclen) != 0) {
      printk("wengine: truncated record\n");
      break;
    }
    derr = wcompact_decode(record, reclen, &fr);
    if (derr != 0) {
      printk("wengine: record refused (code %d)\n", derr);
      refused++;
      continue;
    }
    frames++;
    if (!first && fr.frame_seq != last_seq + 1) {
      uint64_t gap = (fr.frame_seq > last_seq) ? fr.frame_seq - last_seq - 1 : 0;
      missed += gap;
      printk("wengine: gap of %llu before seq %llu\n", gap, fr.frame_seq);
    }
    first = 0;
    last_seq = fr.frame_seq;
    printk("frame seq=%llu step=%llu t=%f ch=%u\n", fr.frame_seq, fr.step_index,
           fr.sim_time, fr.n_channels);
    for (c = 0; c < fr.n_channels; c++) {
      /* Display is float-rounded; the wire doubles stay bit-exact. */
      printk("  %s = %f\n", fr.ch[c].name, fr.ch[c].value);
    }
  }
  zsock_close(fd);
  return 0;
}
