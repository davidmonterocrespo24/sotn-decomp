/* Telemetry that never waits for a listener.
 *
 * The Waveshare board talks over a UART: characters go down the wire whether
 * anyone is attached or not, and a write never blocks for long. The handheld
 * talks over the chip's native USB, and that is a different contract — when no
 * host is draining the endpoint the ring buffer fills and the write BLOCKS,
 * forever.
 *
 * SOTN's steady-state log is only one line per second, which sounds harmless
 * until you do the arithmetic: a 1 KB transmit buffer takes a handful of
 * seconds to fill, and after that the game stops inside a printf. Untethered
 * play is the whole point of a handheld, so this is not an edge case — it is
 * the normal way the console will be used.
 *
 * So printing is best-effort: format into a small buffer and hand it to the
 * USB driver with a zero timeout. With a terminal attached everything appears
 * as before; with nobody listening the bytes are dropped and the game runs at
 * full speed, which is what a console should do anyway.
 *
 * Force-included into every translation unit of the handheld build, so the
 * game's own printf calls are covered without editing the source tree.
 */
#ifndef SOTN_PRINTF_H
#define SOTN_PRINTF_H

#ifdef SOTN_BOARD_XIAO

#include <stdarg.h>
#include <stdio.h>

int Sotn_Printf(const char* fmt, ...);
/* Registered with esp_log_set_vprintf so ESP_LOGx — which goes through the
 * log component's own vprintf hook and never touches printf — cannot block
 * either. The panel and card drivers both log through it at init. */
int Sotn_VPrintf(const char* fmt, va_list ap);

/* Undefined first because some headers in this tree have their own ideas
 * about the name. */
#undef printf
#define printf Sotn_Printf

#endif /* SOTN_BOARD_XIAO */

#endif /* SOTN_PRINTF_H */
