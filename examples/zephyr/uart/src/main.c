/* WEngine UART HIL sample (SIG-8): stream consumer + control client.
 *
 * Reads the host's --emit-uart wire on the console UART, prints every
 * sample, answers nothing (devices only send control on demand), and
 * sends one PING at startup to prove the control round-trip. ASCII and
 * COBS modes share this file (CONFIG_WUART_COBS selects the demux and
 * the TX framing). No C++ STL, no heap: all state is static.
 *
 * Host side (115200 8N1 to match the overlay):
 *   ./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1kHz \
 *     --sig-amp 3.3V --steps 1000 --dt 0.1ms \
 *     --emit-uart /dev/ttyUSB0 --control-uart
 * COBS twin: append --emit-uart-mode cobs and set CONFIG_WUART_COBS=y.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <stdbool.h>

#include "wuart.h"

static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

/* RX heartbeat for single-UART diagnosis (no wire traffic): 1 Hz blink =
 * loop alive; extra toggle per stream line = bytes arriving. Boards
 * without a led0 alias degrade to a no-op. */
static const struct gpio_dt_spec hb_led =
    GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});

#if CONFIG_WUART_COBS
#define WUART_COBS_MODE 1
#else
#define WUART_COBS_MODE 0
#endif

static void tx_bytes(const char *p, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    uart_poll_out(uart, (unsigned char)p[i]);
  }
}

/* Send one control command line (adds framing + delimiter for the mode). */
static void tx_command(const char *line) {
#if WUART_COBS_MODE
  char frame[160];
  char payload[144];
  size_t n;
  size_t i = 0;
  payload[0] = 'Q';
  while (line[i] != '\0' && i + 2 < sizeof(payload)) {
    payload[i + 1] = line[i];
    i++;
  }
  payload[i + 1] = '\0';
  n = wuart_cobs_encode(payload, frame, sizeof(frame));
  if (n > 0 && n < sizeof(frame)) {
    frame[n] = '\0'; /* delimiter byte value zero */
    tx_bytes(frame, n + 1);
  }
#else
  tx_bytes("!", 1);
  {
    size_t n = 0;
    while (line[n] != '\0') {
      n++;
    }
    tx_bytes(line, n);
  }
  tx_bytes("\n", 1);
#endif
}

int main(void) {
  static wuart_demux_t demux;
  uint64_t samples = 0;
  uint64_t malformed = 0;
  uint64_t last_step = 0;
  uint32_t last_hb = 0;
  int first = 1;

  if (!device_is_ready(uart)) {
    printk("wengine: console uart not ready\n");
    return 0;
  }
  /* RX heartbeat for single-UART diagnosis (no wire traffic): 1 Hz blink
   * proves the loop is alive; the stream case below adds one toggle per
   * line so arriving bytes visibly flicker. Absent led0: no-op. */
  const bool hb_ok = gpio_is_ready_dt(&hb_led) &&
                     gpio_pin_configure_dt(&hb_led, GPIO_OUTPUT_ACTIVE) == 0;
  if (hb_ok) {
    printk("wengine: led heartbeat on led0\n");
  }
  wuart_init(&demux, WUART_COBS_MODE);
  printk("wengine: uart hil sample (%s mode, 115200 8N1%s)\n",
         WUART_COBS_MODE ? "cobs" : "ascii",
         IS_ENABLED(CONFIG_WUART_QUIET) ? ", quiet wire" : "");
  /* Prove the control round-trip as soon as the host attaches. */
  tx_command("PING");

  for (;;) {
    unsigned char byte;
    uint32_t now = k_uptime_get_32();
    if (hb_ok && (now - last_hb) >= 1000u) {
      last_hb = now;
      (void)gpio_pin_toggle_dt(&hb_led);
    }
    if (uart_poll_in(uart, &byte) < 0) {
      k_msleep(1);
      continue;
    }
    switch (wuart_feed(&demux, byte)) {
    case WUART_STREAM: {
      wuart_sample_t s;
      if (wuart_parse_sample(demux.text, &s) == 0) {
        uint64_t gap = 0;
        samples++;
        if (!first && s.step != last_step + 1) {
          gap = (s.step > last_step) ? s.step - last_step - 1 : 0;
        }
        first = 0;
        last_step = s.step;
        if (hb_ok) {
          (void)gpio_pin_toggle_dt(&hb_led); /* RX flowing: flicker */
        }
#if !IS_ENABLED(CONFIG_WUART_QUIET)
        /* Float print is single precision on most boards: the wire
         * stays bit-exact (%.17g), the display rounds. Sample-exact
         * verification happens on the host (parse ==, see test). */
        printk("sig id=%llu step=%llu t=%f v=%f %c%s\n", s.sig_id, s.step,
               s.time, s.value, s.qty, gap ? " GAP" : "");
        if (gap) {
          printk("wengine: missed %llu sample(s) before step %llu\n", gap,
                 s.step);
        }
#else
        (void)samples;
        (void)gap;
#endif
      } else {
        malformed++;
      }
      break;
    }
    case WUART_CMD:
    case WUART_RSP:
#if !IS_ENABLED(CONFIG_WUART_QUIET)
      /* Device-side role: the host only ever sends responses here. */
      printk("ctl: %s\n", demux.text);
#endif
      break;
    case WUART_MALFORMED:
      malformed++;
#if !IS_ENABLED(CONFIG_WUART_QUIET)
      if (malformed % 50 == 1) {
        printk("wengine: malformed units=%llu (resynced)\n", malformed);
      }
#else
      (void)malformed;
#endif
      break;
    case WUART_NEED_MORE:
    default:
      break;
    }
    /* Example interaction (uncomment to retune the live run):
     *   tx_command("SET 0 FREQ 50");   -- needs --control-uart on host
     *   tx_command("STOP 0");          -- freeze source serial 0
     */
  }
  return 0;
}
