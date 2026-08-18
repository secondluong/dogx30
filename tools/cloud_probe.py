#!/usr/bin/env python3
"""点云下行通道的端到端校验。

连上网关，订阅点云，把收到的二进制帧解回坐标，检查：
  - 帧头魔数、版本、点数上限
  - 反量化后的坐标是否落在仿真器造出的场景范围内
  - 下行帧率是否被抽到了配置值（而不是原样转发 10 Hz）
  - 退订之后是否真的停发

最后一条最容易写错也最容易被忽略：退订不生效的话，点云会一直占着 MESH 带宽，
而现场未必看得出来 —— 视频会莫名其妙变卡，但没人会想到是点云没停。
"""

import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import time

MAGIC = b"X30C"
HEADER_SIZE = 40


class Ws:
    """够用的 WebSocket 客户端，支持文本与二进制两种帧。"""

    def __init__(self, host, port, timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET / HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode())

        expected = base64.b64encode(hashlib.sha1(
            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()
        ).digest()).decode()

        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("握手期间连接被关闭")
            buf += chunk
        head, _, rest = buf.partition(b"\r\n\r\n")
        if expected.encode() not in head:
            raise RuntimeError("Sec-WebSocket-Accept 不匹配")
        self.buf = rest

    def send(self, obj):
        payload = json.dumps(obj, ensure_ascii=False).encode()
        mask = os.urandom(4)
        n = len(payload)
        frame = bytearray([0x81])
        if n < 126:
            frame.append(0x80 | n)
        elif n < 65536:
            frame.append(0x80 | 126)
            frame += struct.pack(">H", n)
        else:
            frame.append(0x80 | 127)
            frame += struct.pack(">Q", n)
        frame += mask
        frame += bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(bytes(frame))

    def recv(self):
        """返回 (是否二进制, 负载)。超时抛 socket.timeout。"""
        while True:
            while len(self.buf) < 2:
                self._fill()
            b0, b1 = self.buf[0], self.buf[1]
            opcode = b0 & 0x0F
            length = b1 & 0x7F
            offset = 2
            if length == 126:
                while len(self.buf) < 4:
                    self._fill()
                length = struct.unpack(">H", self.buf[2:4])[0]
                offset = 4
            elif length == 127:
                while len(self.buf) < 10:
                    self._fill()
                length = struct.unpack(">Q", self.buf[2:10])[0]
                offset = 10
            while len(self.buf) < offset + length:
                self._fill()
            payload = self.buf[offset:offset + length]
            self.buf = self.buf[offset + length:]
            if opcode in (0x1, 0x2):
                return opcode == 0x2, payload
            if opcode == 0x8:
                raise RuntimeError("服务端关闭了连接")

    def _fill(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise RuntimeError("连接被关闭")
        self.buf += chunk

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def decode_cloud(payload):
    if len(payload) < HEADER_SIZE:
        raise ValueError(f"帧太短: {len(payload)} 字节")
    if payload[:4] != MAGIC:
        raise ValueError(f"魔数不对: {payload[:4]!r}")
    version = payload[4]
    flags = payload[5]
    seq, stamp_ms = struct.unpack("<IQ", payload[8:20])
    ox, oy, oz, scale = struct.unpack("<ffff", payload[20:36])
    count = struct.unpack("<I", payload[36:40])[0]

    body = payload[HEADER_SIZE:]
    if len(body) != count * 6:
        raise ValueError(
            f"点数据长度不符: 声明 {count} 点应为 {count * 6} 字节，实得 {len(body)}")

    pts = []
    for i in range(count):
        qx, qy, qz = struct.unpack_from("<HHH", body, i * 6)
        pts.append((ox + qx * scale, oy + qy * scale, oz + qz * scale))
    return {
        "version": version, "flags": flags, "seq": seq,
        "stamp_ms": stamp_ms, "scale": scale, "count": count,
        "origin": (ox, oy, oz), "points": pts,
    }


class Checker:
    def __init__(self):
        self.passed = 0
        self.failed = 0

    def check(self, name, ok, detail=""):
        if ok:
            self.passed += 1
            print(f"  \033[32m通过\033[0m {name}")
        else:
            self.failed += 1
            print(f"  \033[31m失败\033[0m {name} {detail}")


def main():
    parser = argparse.ArgumentParser(description="点云下行通道校验")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--expect-hz", type=float, default=2.0)
    parser.add_argument("--max-points", type=int, default=20000)
    args = parser.parse_args()

    c = Checker()
    ws = Ws(args.host, args.port, timeout=8.0)

    # 连上先把 hello/state/media_plan 这些排空
    deadline = time.time() + 1.0
    ws.sock.settimeout(0.3)
    while time.time() < deadline:
        try:
            ws.recv()
        except (socket.timeout, TimeoutError):
            break
    ws.sock.settimeout(1.0)

    print("\n订阅点云")
    ws.send({"t": "cloud_sub"})

    status = None
    frames = []
    start = time.time()
    # 给感知主机发现 + 握手留出时间，再收够几帧用来算频率
    while time.time() - start < 10.0 and len(frames) < 6:
        try:
            is_bin, payload = ws.recv()
        except (socket.timeout, TimeoutError):
            continue
        if is_bin:
            frames.append((time.time(), payload))
        else:
            msg = json.loads(payload)
            if msg.get("t") == "cloud_status":
                status = msg
            elif msg.get("t") == "error":
                print(f"  服务端报错: {msg.get('code')} {msg.get('message')}")

    c.check("收到点云二进制帧", len(frames) > 0,
            f"（8 秒内一帧都没收到，status={status}）")
    if not frames:
        ws.close()
        print(f"\n通过 {c.passed}，失败 {c.failed}")
        return 1

    decoded = []
    for _, payload in frames:
        try:
            decoded.append(decode_cloud(payload))
        except ValueError as e:
            c.check("帧格式合法", False, str(e))
            ws.close()
            return 1
    c.check("帧格式合法", True)
    c.check("版本号为 1", decoded[0]["version"] == 1,
            f"（实为 {decoded[0]['version']}）")

    counts = [d["count"] for d in decoded]
    c.check(f"点数不超过上限 {args.max_points}",
            all(n <= args.max_points for n in counts),
            f"（实测 {counts}）")
    c.check("点数非零", all(n > 0 for n in counts), f"（实测 {counts}）")

    # 仿真器造的场景在 ±4.5 m 的盒子里。超出说明反量化或坐标处理错了。
    pts = decoded[0]["points"]
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    zs = [p[2] for p in pts]
    in_box = (min(xs) > -5 and max(xs) < 5 and
              min(ys) > -5 and max(ys) < 5 and
              min(zs) > -1.5 and max(zs) < 3)
    c.check("反量化坐标落在仿真场景范围内", in_box,
            f"（x[{min(xs):.2f},{max(xs):.2f}] "
            f"y[{min(ys):.2f},{max(ys):.2f}] z[{min(zs):.2f},{max(zs):.2f}]）")

    # 量化精度：scale 应该远小于体素边长，否则量化本身就成了主要误差源
    c.check("量化精度优于 1 cm", decoded[0]["scale"] < 0.01,
            f"（scale={decoded[0]['scale'] * 1000:.2f} mm）")

    c.check("序号递增", all(decoded[i]["seq"] < decoded[i + 1]["seq"]
                            for i in range(len(decoded) - 1)),
            f"（{[d['seq'] for d in decoded]}）")

    if len(frames) >= 4:
        span = frames[-1][0] - frames[0][0]
        hz = (len(frames) - 1) / span if span > 0 else 0
        # 上界卡死：抽帧没生效的话这里会是 10 Hz，带宽直接翻五倍
        c.check(f"下行帧率被抽到约 {args.expect_hz} Hz",
                hz <= args.expect_hz * 1.6,
                f"（实测 {hz:.1f} Hz，抽帧可能没生效）")

    per_frame = len(frames[0][1])
    kbps = per_frame * args.expect_hz * 8 / 1000
    print(f"  单帧 {per_frame / 1024:.0f} KB，"
          f"{args.expect_hz} Hz 约 {kbps:.0f} kbps")
    c.check("带宽在 3 Mbps 预算内", kbps < 3000, f"（实测 {kbps:.0f} kbps）")

    print("\n退订")
    ws.send({"t": "cloud_unsub"})

    # 这两个循环都必须按时间收口，不能等超时 —— 网关的遥测是 10 Hz 文本广播，
    # 一直有东西可收，靠 socket 超时退出会永远转下去。
    drain_until = time.time() + 0.6
    ws.sock.settimeout(0.3)
    while time.time() < drain_until:
        try:
            ws.recv()
        except (socket.timeout, TimeoutError):
            break

    stopped = True
    watch_until = time.time() + 2.5
    while time.time() < watch_until:
        try:
            is_bin, _ = ws.recv()
        except (socket.timeout, TimeoutError):
            continue
        if is_bin:
            stopped = False
            break
    c.check("退订后不再下发点云", stopped, "（还在收到帧，带宽会被一直占着）")

    ws.close()
    print(f"\n通过 {c.passed}，失败 {c.failed}")
    return 0 if c.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
