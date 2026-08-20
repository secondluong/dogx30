#!/usr/bin/env python3
"""启动感知主机上的原厂 LIO（API 1.5.2.4 / 附录 C）。

往 192.168.1.105:60000 发简单指令 0x0BAA0001，value=1 开、0 关。
狗要站稳，否则感知主机会回 -1。
"""

import argparse
import socket
import struct
import sys
import time

CODE = 0x0BAA0001


def rewrite_rosrpc(uri):
    u = uri.replace("rosrpc://", "")
    host, port = u.rsplit(":", 1)
    if host in ("host", "localhost"):
        host = "192.168.1.105"
    return host, int(port)


def fields(**kv):
    body = b""
    for k, v in kv.items():
        f = f"{k}={v}".encode()
        body += struct.pack("<I", len(f)) + f
    return struct.pack("<I", len(body)) + body


def recv_exact(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError()
        b += c
    return b


def call_lio_enable(master, on=True):
    import xmlrpc.client
    m = xmlrpc.client.ServerProxy(master)
    code, msg, uri = m.lookupService("/x30_lio", "/lio_enable")
    if code != 1:
        return False, msg
    host, port = rewrite_rosrpc(uri)
    s = socket.create_connection((host, port), timeout=2)
    s.sendall(fields(
        callerid="/x30_lio", service="/lio_enable",
        type="std_srvs/SetBool",
        md5sum="09fb03525b03e7ea1fd3992bafd87e16",
        persistent="0",
    ))
    hlen = struct.unpack("<I", recv_exact(s, 4))[0]
    recv_exact(s, hlen)
    s.sendall(struct.pack("<I", 1) + bytes([1 if on else 0]))
    ok = recv_exact(s, 1)[0]
    s.close()
    return ok == 1, "ok" if ok == 1 else "service rejected"


def send(ip, port, on, timeout=1.5):
    pkt = struct.pack("<III", CODE, 1 if on else 0, 0)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.sendto(pkt, (ip, port))
    try:
        data, addr = s.recvfrom(64)
    except socket.timeout:
        s.close()
        return None, "超时无应答"
    s.close()
    if len(data) < 12:
        return None, f"应答过短 {len(data)}B from {addr}"
    code, value, typ = struct.unpack_from("<III", data, 0)
    return (code, value, typ), None


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--ip", default="192.168.1.105")
    p.add_argument("--port", type=int, default=60000)
    p.add_argument("--off", action="store_true")
    p.add_argument("--wait", type=float, default=0,
                   help="感知主机不在线时最多等多少秒")
    args = p.parse_args()

    deadline = time.time() + args.wait
    while True:
        try:
            reply, err = send(args.ip, args.port, not args.off)
        except OSError as e:
            reply, err = None, str(e)
        if reply is not None:
            code, value, typ = reply
            ok = value == 0
            print(f"{'OK' if ok else 'FAIL'}  UDP 0x{code:08x} value={value} "
                  f"({ '成功' if ok else '拒绝，狗要站稳或先退多帧地形图' })")
            if not ok:
                return 2
            en_ok, en_msg = call_lio_enable(f"http://{args.ip}:11311")
            print(f"{'OK' if en_ok else 'FAIL'}  /lio_enable {en_msg}")
            return 0 if en_ok else 3
        if time.time() >= deadline:
            print(f"FAIL  {err}", file=sys.stderr)
            return 1
        time.sleep(2)


if __name__ == "__main__":
    sys.exit(main())
