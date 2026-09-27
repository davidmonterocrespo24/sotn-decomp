# SOTN ESP32-S3 - play from the PC keyboard over the serial port.
#
#   python esp32/play.py [COM6]
#
# Keys: WASD = d-pad | X = Cross (jump) | Z = Square (attack)
#       Q = Triangle | E = Circle | 1/2/3/4 = L1/R1/L2/R2
#       Enter = START | Space = SELECT | ESC = quit
# Hold the key down: keyboard auto-repeat keeps the button held.
import sys
import time
import msvcrt
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"


def conectar():
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.dtr = False  # open with DTR/RTS low, or the board resets
    s.rts = False
    s.timeout = 0
    s.open()
    time.sleep(0.2)
    # The ESP32-S3 USB-Serial/JTAG DISCARDS host->device bytes while DTR is
    # low (it is the CDC "terminal ready" line). With RTS low, raising DTR
    # does not reset the board, and it enables the keyboard.
    s.dtr = True
    return s


s = conectar()
print(f"Connected to {port}. WASD move, X jump, Z attack, Enter START, ESC quit.")

try:
    while True:
        # PC keys -> board
        while msvcrt.kbhit():
            ch = msvcrt.getch()
            if ch == b"\x1b":  # ESC
                raise KeyboardInterrupt
            if ch in (b"\x00", b"\xe0"):  # special-key prefix (arrows)
                arrow = msvcrt.getch()
                ch = {b"H": b"w", b"P": b"s", b"K": b"a", b"M": b"d"}.get(arrow, b"")
            if ch:
                s.write(ch)
        # board log -> console (with reconnect: Windows' CDC driver wedges
        # reads after a while; the board itself keeps running fine)
        try:
            data = s.read(4096)
        except serial.SerialException:
            try:
                s.close()
            except Exception:
                pass
            time.sleep(1)
            try:
                s = conectar()
                print("[reconnected]")
            except Exception:
                continue
            data = b""
        if data:
            sys.stdout.write(data.decode(errors="replace"))
            sys.stdout.flush()
except KeyboardInterrupt:
    pass
finally:
    s.close()
    print("\nDisconnected.")
