#!/usr/bin/env python3
"""往本机气体口送 3A 02 十路浓度，用来对网页。

默认打到 127.0.0.1:1000，约每 2 秒一帧，和实机主板同一拍。

    python3 tools/gas_sim.py
    python3 tools/gas_sim.py --host 192.168.1.120 --port 1000
"""

from __future__ import annotations

import argparse
import math
import socket
import struct
import time


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc & 0xFFFF


def be_float(v: float) -> bytes:
    return struct.pack(">f", v)


def build_frame(tick: float) -> bytes:
    # 端口 1–10，类型按协议表轮一圈常见组合。
    types = [2, 3, 6, 7, 1, 9, 4, 10, 8, 5]  # O2 CO H2S LEL CH4 VOC NH3 SO2 H2 NO2
    values = [
        20.9,
        8.0 + 4.0 * math.sin(tick),
        1.2,
        3.5,
        2.0,
        0.40,
        5.0,
        0.8,
        40.0,
        0.15,
    ]
    buf = bytearray(b"\x3a\x02\x00")
    for i in range(10):
        port = i + 1
        gid = types[i]
        if i == 3 and int(tick) % 8 == 0:
            buf += bytes((port, gid, 0xFF, 0xFF, 0xFF, 0xFF))  # 偶发掉线
        else:
            buf += bytes((port, gid)) + be_float(values[i])
    crc = crc16_modbus(buf)
    buf += bytes((crc >> 8, crc & 0xFF))
    return bytes(buf)


def main() -> None:
    p = argparse.ArgumentParser(description="气体 3A 02 UDP 仿真")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=1000)
    p.add_argument("--interval", type=float, default=2.0)
    args = p.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    t0 = time.time()
    n = 0
    print(f"向 {args.host}:{args.port} 发送 3A 02，Ctrl-C 停")
    try:
        while True:
            frame = build_frame(time.time() - t0)
            sock.sendto(frame, (args.host, args.port))
            n += 1
            if n == 1 or n % 5 == 0:
                print(f"  已发 {n} 帧  {len(frame)} 字节")
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print(f"\n停止，共 {n} 帧")


if __name__ == "__main__":
    main()
