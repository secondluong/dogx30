#!/usr/bin/env python3
"""假的 ROS1 master + 发布者，专供点云链路离线自测。

机器狗是生产设备，感知主机能不能连通到现场才知道。没有它就没法验证
XML-RPC 发现、TCPROS 握手、PointCloud2 解析、降采样、量化下行这一整条链路，
调试全要压到装机当天 —— 这个仿真器就是为了把那一天的工作量提前拿掉。

它实现的是真 ROS 的线上协议，不是我们自己约定的简化版，所以在这里能跑通，
到现场大概率也能跑通。已知的差异只有一处：真 master 会主动回调
publisherUpdate，这里不回调（我们的从节点实现了，但没有触发它的场景）。

用法:
    python3 tools/ros_sim.py                    # 监听 11311，10 Hz 发布
    python3 tools/ros_sim.py --port 11400       # 换端口，避开真 master
    python3 tools/ros_sim.py --points 120000    # 每帧点数
"""

import argparse
import math
import random
import socket
import struct
import sys
import threading
import time
from xmlrpc.server import SimpleXMLRPCServer

POINTCLOUD2_MD5 = "1158d486dd51d683ce2f1be655c3c181"
POINTCLOUD2_TYPE = "sensor_msgs/PointCloud2"


def ros_string(s: str) -> bytes:
    raw = s.encode("utf-8")
    return struct.pack("<I", len(raw)) + raw


def build_scene(n_points: int, phase: float):
    """造一个能一眼看出对不对的场景：走廊 + 地面 + 两个障碍 + 一面转动的墙。

    纯随机点云看不出坐标系有没有搞错，也看不出量化有没有失真。
    有结构的场景在渲染端一眼就能发现问题。
    """
    pts = []

    # 地面：8m x 8m
    n_floor = n_points // 3
    for _ in range(n_floor):
        pts.append((random.uniform(-4, 4), random.uniform(-4, 4),
                    -0.45 + random.gauss(0, 0.01)))

    # 两侧墙，形成走廊
    n_wall = n_points // 3
    for _ in range(n_wall):
        y = 2.0 if random.random() < 0.5 else -2.0
        pts.append((random.uniform(-4, 4), y + random.gauss(0, 0.01),
                    random.uniform(-0.45, 2.0)))

    # 前方两个箱子障碍
    n_box = n_points // 6
    for _ in range(n_box):
        cx = 2.5 if random.random() < 0.5 else 3.5
        cy = 0.8 if cx > 3 else -0.8
        pts.append((cx + random.uniform(-0.3, 0.3),
                    cy + random.uniform(-0.3, 0.3),
                    random.uniform(-0.45, 0.35)))

    # 一堵绕原点转的墙，用来确认帧确实在更新而不是卡住了
    n_rot = n_points - len(pts)
    for _ in range(max(0, n_rot)):
        r = random.uniform(3.0, 3.4)
        a = phase + random.uniform(-0.25, 0.25)
        pts.append((r * math.cos(a), r * math.sin(a),
                    random.uniform(-0.45, 1.2)))

    return pts


def serialize_pointcloud2(seq: int, stamp: float, points) -> bytes:
    """按 sensor_msgs/PointCloud2 的 ROS1 序列化格式打包。

    字段布局刻意做成 x,y,z,intensity 共 16 字节，和 Livox 驱动的实际输出一致，
    这样才能验证解析器真的会按 offset 跳字段，而不是假设紧凑排列。
    """
    sec = int(stamp)
    nsec = int((stamp - sec) * 1e9)

    out = bytearray()
    out += struct.pack("<III", seq, sec, nsec)
    out += ros_string("livox_frame")
    out += struct.pack("<II", 1, len(points))  # height, width

    fields = [("x", 0, 7, 1), ("y", 4, 7, 1), ("z", 8, 7, 1),
              ("intensity", 12, 7, 1)]
    out += struct.pack("<I", len(fields))
    for name, offset, datatype, count in fields:
        out += ros_string(name)
        out += struct.pack("<IBI", offset, datatype, count)

    point_step = 16
    row_step = point_step * len(points)
    out += struct.pack("<BII", 0, point_step, row_step)

    data = bytearray()
    for x, y, z in points:
        data += struct.pack("<ffff", x, y, z, 100.0)
    out += struct.pack("<I", len(data))
    out += data
    out += struct.pack("<B", 1)  # is_dense
    return bytes(out)


class TcprosPublisher(threading.Thread):
    """TCPROS 服务端：握手校验 md5，然后按固定频率推消息。"""

    def __init__(self, topic, rate_hz, n_points):
        super().__init__(daemon=True)
        self.topic = topic
        self.rate_hz = rate_hz
        self.n_points = n_points
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", 0))
        self.sock.listen(4)
        self.port = self.sock.getsockname()[1]
        self.running = True

    def run(self):
        while self.running:
            try:
                conn, addr = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self.serve, args=(conn, addr),
                             daemon=True).start()

    def serve(self, conn, addr):
        try:
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            header = self.read_header(conn)
            if header is None:
                conn.close()
                return

            print(f"[ros_sim] 订阅者 {addr[0]}:{addr[1]} 握手: "
                  f"callerid={header.get('callerid')} "
                  f"topic={header.get('topic')}")

            # 真发布者在 md5 不符时会回一个带 error= 的头再断开。
            # 这里照做，否则我们自己那条错误路径永远测不到。
            if header.get("md5sum") not in (POINTCLOUD2_MD5, "*"):
                print("[ros_sim] md5sum 不匹配，拒绝连接")
                self.write_header(conn, {
                    "error": "md5sums do not match",
                })
                conn.close()
                return

            self.write_header(conn, {
                "md5sum": POINTCLOUD2_MD5,
                "type": POINTCLOUD2_TYPE,
                "callerid": "/ros_sim",
                "latching": "0",
            })

            seq = 0
            period = 1.0 / self.rate_hz
            next_at = time.time()
            while self.running:
                pts = build_scene(self.n_points, phase=seq * 0.06)
                msg = serialize_pointcloud2(seq, time.time(), pts)
                conn.sendall(struct.pack("<I", len(msg)) + msg)
                seq += 1
                next_at += period
                delay = next_at - time.time()
                if delay > 0:
                    time.sleep(delay)
                else:
                    next_at = time.time()
        except (OSError, BrokenPipeError):
            print(f"[ros_sim] 订阅者 {addr[0]}:{addr[1]} 断开")
        finally:
            try:
                conn.close()
            except OSError:
                pass

    @staticmethod
    def read_header(conn):
        raw = TcprosPublisher.recv_exact(conn, 4)
        if raw is None:
            return None
        total = struct.unpack("<I", raw)[0]
        if total > 65536:
            return None
        body = TcprosPublisher.recv_exact(conn, total)
        if body is None:
            return None
        fields = {}
        pos = 0
        while pos + 4 <= len(body):
            n = struct.unpack("<I", body[pos:pos + 4])[0]
            pos += 4
            kv = body[pos:pos + n].decode("utf-8", "replace")
            pos += n
            if "=" in kv:
                k, v = kv.split("=", 1)
                fields[k] = v
        return fields

    @staticmethod
    def write_header(conn, fields):
        body = b""
        for k, v in fields.items():
            kv = f"{k}={v}".encode("utf-8")
            body += struct.pack("<I", len(kv)) + kv
        conn.sendall(struct.pack("<I", len(body)) + body)

    @staticmethod
    def recv_exact(conn, n):
        buf = b""
        while len(buf) < n:
            chunk = conn.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    def stop(self):
        self.running = False
        try:
            self.sock.close()
        except OSError:
            pass


def main():
    parser = argparse.ArgumentParser(description="ROS1 点云仿真发布者")
    parser.add_argument("--port", type=int, default=11311,
                        help="master 端口，默认 11311")
    parser.add_argument("--topic", default="/lidar_points")
    parser.add_argument("--rate", type=float, default=10.0,
                        help="发布频率，默认 10 Hz，与真机一致")
    parser.add_argument("--points", type=int, default=60000,
                        help="每帧点数。真机四台 Mid-360 合起来约 8 万")
    parser.add_argument("--host", default="127.0.0.1",
                        help="回给订阅者的地址")
    args = parser.parse_args()

    pub = TcprosPublisher(args.topic, args.rate, args.points)
    pub.start()

    # 发布者节点的 XML-RPC 从接口，回答 requestTopic。
    node = SimpleXMLRPCServer((args.host, 0), logRequests=False,
                              allow_none=True)
    node_port = node.server_address[1]
    node_uri = f"http://{args.host}:{node_port}/"

    def request_topic(caller_id, topic, protocols):
        print(f"[ros_sim] requestTopic({caller_id}, {topic}) "
              f"-> TCPROS {args.host}:{pub.port}")
        return [1, "ready", ["TCPROS", args.host, pub.port]]

    node.register_function(request_topic, "requestTopic")
    node.register_function(lambda caller_id: [1, "ok", 1234], "getPid")
    threading.Thread(target=node.serve_forever, daemon=True).start()

    # master。只实现订阅者会用到的那两个方法。
    master = SimpleXMLRPCServer((args.host, args.port), logRequests=False,
                                allow_none=True)

    def register_subscriber(caller_id, topic, topic_type, caller_api):
        print(f"[ros_sim] registerSubscriber({caller_id}, {topic}, "
              f"{topic_type}) 来自 {caller_api}")
        if topic != args.topic:
            return [1, "no publishers", []]
        return [1, "registered", [node_uri]]

    def unregister_subscriber(caller_id, topic, caller_api):
        print(f"[ros_sim] unregisterSubscriber({caller_id}, {topic})")
        return [1, "unregistered", 1]

    master.register_function(register_subscriber, "registerSubscriber")
    master.register_function(unregister_subscriber, "unregisterSubscriber")
    master.register_function(
        lambda caller_id, subgraph="": [
            1, "ok", [[args.topic, POINTCLOUD2_TYPE]]],
        "getPublishedTopics")
    master.register_function(lambda caller_id: [1, "ok", 1234], "getPid")

    print(f"[ros_sim] master     http://{args.host}:{args.port}/")
    print(f"[ros_sim] 发布者节点 {node_uri}")
    print(f"[ros_sim] TCPROS     {args.host}:{pub.port}")
    print(f"[ros_sim] 话题 {args.topic}，{args.rate} Hz，"
          f"{args.points} 点/帧")
    print("[ros_sim] Ctrl-C 退出")

    try:
        master.serve_forever()
    except KeyboardInterrupt:
        print("\n[ros_sim] 退出")
    finally:
        pub.stop()


if __name__ == "__main__":
    sys.exit(main())
