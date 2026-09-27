# SOTN on the handheld (XIAO ESP32S3 Sense)

The same firmware serves two machines. `build.ps1` with no argument builds for
the Waveshare development board; `build.ps1 xiao` builds for the handheld. They
share everything except the board layer.

|                 | Waveshare              | XIAO handheld            |
|-----------------|------------------------|--------------------------|
| Panel           | ST7789 at 80 MHz       | ILI9341 at 40 MHz        |
| Flash           | 16 MB                  | 8 MB                     |
| Game data       | internal FAT partition | **microSD**              |
| Serial console  | UART                   | native USB               |
| Controls        | 10 direct pins         | stick + ladder + 2 pins  |

Wiring and parts: [`hardware/HARDWARE.md`](hardware/HARDWARE.md).

## Preparing the card

On the handheld **all the game data lives on the microSD**, not just the music:
8 MB of flash cannot hold a 3 MB app and the ~4 MB data image side by side.
After extracting your own disc, copy the contents of `esp32/fatfs_root/` to the
**root** of the card, so it looks like this:

```
/disks/us/BIN/...
/disks/us/ST/NP3/...
/disks/us/ST/NZ0/...
/disks/us/ST/SEL/...
/disks/us/ST/WRP/...
/disks/us/VAB/...
/stage.txt
```

`stage.txt` picks the boot stage. `nz0` jumps straight into the Alchemy
Laboratory — the best first boot, because if something is wrong you see it
without sitting through the logos. The word `boot` runs the normal sequence
(logos, title, save menu).

Music is separate and optional: put `sotn.us.cue` and its `.bin` on the card
root and the XA tracks play. Without them the game runs silent. They are 583 MB,
so they cannot live anywhere else.

The card can be shared with the Metal Gear Solid port: that one uses `/MGS/`,
this one `/disks/`.

If the data is missing the firmware says so on the serial console instead of
failing deep inside the stage loader.

## Build and flash

```powershell
python esp32/const_stage_tables.py
powershell -ExecutionPolicy Bypass -File esp32/build.ps1 xiao
powershell -ExecutionPolicy Bypass -File esp32/flash.ps1 COM9 -Xiao
```

Each board has its own build directory and its own generated `sdkconfig`. That
matters: IDF generates `sdkconfig` in the project root on the first build and
from then on **ignores** the defaults file, so without the split the second
board silently inherits the first one's flash size and partition table.

## Controls

The pad bit order is the same as on the other board (both are psyz); only the
source changes. Six buttons share one pin through a resistor ladder, which can
report only **one** press at a time, so the two buttons that must work together
get dedicated pins. In this game that pair is **attack and jump**. Full map in
`hardware/HARDWARE.md`.

The stick calibrates at boot: whatever it reads then is the centre, and it
learns its travel as you move it. Don't touch it while powering on.

## Two traps on this board

**GPIO43 is the panel's data/command pin and also UART0 TX.** Nothing may
install the UART0 driver on this board: doing so hands the pin back to the
serial peripheral, the panel stops telling commands from data, and it goes
blank with nothing in the log to explain why. The console is native USB, and
boot installs that driver before printing anything.

**Printing over USB blocks when nobody is listening.** With a terminal attached
you never notice; unplugged, the buffer fills in a few seconds and the game
stops inside a `printf`. Since playing untethered is the whole point of a
handheld, all output goes through a path that drops bytes when nobody reads
them (`main/sotn_printf.h`), IDF logging included.

## Display pipeline

At 40 MHz a full 256×240 frame takes ~25 ms to send, longer than the game's
16.7 ms frame. Two consequences, both handled in `main/lcd_esp32.c`:

- The finished frame is **copied** before it is sent, so the game can start the
  next one without the panel transmitting a half-redrawn buffer (that was the
  flicker).
- Each 8-line band is **fingerprinted**, and bands identical to what the panel
  already shows are not sent. The HUD and anything that did not move cost
  nothing; the screen runs at ~55 fps when the camera is still.
