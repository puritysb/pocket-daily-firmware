#!/usr/bin/env python3
"""Developer USB bootstrap: request the existing one-shot Same Wi-Fi return.

Opening native USB can reset the reader. This command is only understood by
ENABLE_DEV_REMOTE_FLASH images; it sends no credentials and writes no firmware.
"""
import argparse
import time
import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    args = parser.parse_args()
    connection = serial.Serial()
    connection.port = args.port
    connection.baudrate = 115200
    connection.timeout = 0.25
    connection.dtr = False
    connection.rts = False
    connection.open()
    try:
        time.sleep(8)  # Native USB open may restart the application.
        connection.reset_input_buffer()
        connection.write(b'CMD:DEV_SYNC_STA\n')
        connection.flush()
        deadline = time.monotonic() + 20
        observed = bytearray()
        while time.monotonic() < deadline:
            try:
                observed.extend(connection.read(1024))
            except serial.SerialException:
                break  # reboot can re-enumerate native USB
            if b'Same Wi-Fi return armed' in observed:
                print('Verified Same Wi-Fi return marker; reader restarting')
                return
        raise RuntimeError('No developer USB acknowledgement; inspect reader before retrying')
    finally:
        connection.close()


if __name__ == '__main__':
    main()
