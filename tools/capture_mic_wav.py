#!/usr/bin/env python3
import binascii
import os
import sys
import termios
import time


PORT = "/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_A4:CB:8F:D6:4E:68-if00"
OUT = "/home/paulo/Downloads/esp32_mic_capture.wav"


def configure_raw(fd):
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = attrs[2] | termios.CLOCAL | termios.CREAD
    attrs[3] = 0
    attrs[6][termios.VMIN] = 1
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else PORT
    out = sys.argv[2] if len(sys.argv) > 2 else OUT

    fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
    configure_raw(fd)
    os.write(fd, b"VOICE_READY\n")
    print(f"Aguardando audio do aparelho em {port}...")

    capturing = False
    hex_parts = []
    line = bytearray()
    started_at = time.time()

    try:
        while True:
            b = os.read(fd, 1)
            if not b:
                continue
            if b not in (b"\n", b"\r"):
                line.extend(b)
                continue

            text = line.decode("ascii", errors="ignore").strip()
            line.clear()
            if not text:
                continue

            if text.startswith("AUDIO_WAV_BIN_BEGIN"):
                parts = text.split()
                if len(parts) != 2:
                    continue
                total = int(parts[1])
                data = bytearray()
                started_at = time.time()
                print("Recebendo audio...")
                while len(data) < total:
                    chunk = os.read(fd, min(8192, total - len(data)))
                    if chunk:
                        data.extend(chunk)
                with open(out, "wb") as f:
                    f.write(data)
                print(f"Arquivo salvo: {out}")
                print(f"Tamanho: {len(data)} bytes em {time.time() - started_at:.1f}s")
                return

            if text == "AUDIO_WAV_HEX_BEGIN":
                capturing = True
                hex_parts.clear()
                started_at = time.time()
                print("Recebendo audio...")
                continue

            if text == "AUDIO_WAV_HEX_END" and capturing:
                data = binascii.unhexlify("".join(hex_parts))
                with open(out, "wb") as f:
                    f.write(data)
                print(f"Arquivo salvo: {out}")
                print(f"Tamanho: {len(data)} bytes em {time.time() - started_at:.1f}s")
                return

            if capturing:
                if all(c in "0123456789abcdefABCDEF" for c in text):
                    hex_parts.append(text)
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
