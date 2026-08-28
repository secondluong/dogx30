#!/usr/bin/env python3
"""遥控协议端到端探针。

刻意手写 RFC 6455 客户端而不用 websockets 库：一是免去部署依赖，二是用一份
独立实现去校验网关的帧处理，比用同一套代码自测更有意义。

用法：
    python3 tools/ws_probe.py [--host 127.0.0.1] [--port 8080]

覆盖：HTTP 静态文件、握手、hello、控制权仲裁、状态机指令、速度通道、
急停免鉴权、断连自动释放控制权。
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

FAILURES = []


def check(label, ok, detail=""):
    mark = "PASS" if ok else "FAIL"
    print(f"  [{mark}] {label}" + (f"  {detail}" if detail else ""))
    if not ok:
        FAILURES.append(label)
    return ok


class WsClient:
    def __init__(self, host, port, path="/ws"):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode())

        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("握手期间连接被关闭")
            self.buf += chunk
        head, _, rest = self.buf.partition(b"\r\n\r\n")
        self.buf = rest

        head_text = head.decode("latin1")
        if "101" not in head_text.split("\r\n")[0]:
            raise RuntimeError(f"握手失败: {head_text.splitlines()[0]}")

        expect = base64.b64encode(
            hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
        ).decode()
        got = ""
        for line in head_text.split("\r\n")[1:]:
            if line.lower().startswith("sec-websocket-accept:"):
                got = line.split(":", 1)[1].strip()
        self.accept_ok = got == expect

    def send(self, obj):
        payload = json.dumps(obj).encode()
        mask = os.urandom(4)
        n = len(payload)
        frame = bytearray([0x81])
        if n < 126:
            frame.append(0x80 | n)
        elif n <= 0xFFFF:
            frame.append(0x80 | 126)
            frame += struct.pack(">H", n)
        else:
            frame.append(0x80 | 127)
            frame += struct.pack(">Q", n)
        frame += mask
        frame += bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(bytes(frame))

    def _pop_frame(self):
        """从缓冲区取出一个完整帧；数据不足返回 None。"""
        if len(self.buf) < 2:
            return None
        if self.buf[1] & 0x80:
            raise RuntimeError("服务端不应对帧加掩码")
        length = self.buf[1] & 0x7F
        offset = 2
        if length == 126:
            if len(self.buf) < 4:
                return None
            length = struct.unpack(">H", self.buf[2:4])[0]
            offset = 4
        elif length == 127:
            if len(self.buf) < 10:
                return None
            length = struct.unpack(">Q", self.buf[2:10])[0]
            offset = 10
        if len(self.buf) < offset + length:
            return None
        opcode = self.buf[0] & 0x0F
        payload = self.buf[offset:offset + length]
        self.buf = self.buf[offset + length:]
        if opcode == 0x8:
            raise RuntimeError("服务端发送了 close")
        if opcode != 0x1:
            return self._pop_frame()
        return json.loads(payload.decode())

    def recv(self, timeout=3.0):
        deadline = time.time() + timeout
        while True:
            msg = self._pop_frame()
            if msg is not None:
                return msg
            remaining = deadline - time.time()
            if remaining <= 0:
                raise TimeoutError("recv 超时")
            self.sock.settimeout(remaining)
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("连接已关闭")
            self.buf += chunk

    def drain(self):
        """丢弃所有已到达的消息。

        状态以 10 Hz 持续推送，若不清空，读到的会是几秒前的旧快照 ——
        断言的就成了历史值而非当前值。
        """
        self.sock.settimeout(0.05)
        try:
            while True:
                chunk = self.sock.recv(65536)
                if not chunk:
                    break
                self.buf += chunk
        except OSError:
            pass
        while self._pop_frame() is not None:
            pass

    def wait_for(self, kind, timeout=5.0, predicate=None, keepalive=True):
        # 真实遥控端会持续发心跳来续租控制权。等待期间不发心跳的话，
        # 一个耗时几秒的动作（比如起立）就会让控制权在中途过期。
        deadline = time.time() + timeout
        while time.time() < deadline:
            if keepalive:
                self.ping()
            msg = self.recv(timeout=max(0.1, deadline - time.time()))
            if msg.get("t") == kind and (predicate is None or predicate(msg)):
                return msg
        raise TimeoutError(f"等待 {kind} 超时")

    def ping(self):
        # 限速到 2 Hz。不限的话 ping/pong 会形成一个空转的紧循环。
        now = time.time()
        if now - getattr(self, "last_ping", 0.0) < 0.5:
            return
        self.last_ping = now
        self.seq = getattr(self, "seq", 0) + 1
        self.send({"t": "ping", "id": self.seq})

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def http_get(host, port, path):
    with socket.create_connection((host, port), timeout=5) as s:
        s.sendall(f"GET {path} HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n".encode())
        data = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    head, _, body = data.partition(b"\r\n\r\n")
    status = head.decode("latin1").splitlines()[0]
    return status, body


def no_terrain_scenario(host, port):
    """地形图主机不可达时，楼梯步态必须报错，而不是假装成功。

    这是实机上最容易踩的坑：只发步态指令，运动主机静默忽略，界面上按钮亮了
    但狗不上楼。这一项验证网关能把这种情况识别出来并给出可操作的提示。
    """
    print("\n== 地形图不可达 ==")
    a = WsClient(host, port)
    a.wait_for("hello")
    a.send({"t": "claim"})
    a.wait_for("control", predicate=lambda m: "granted" in m)

    a.send({"t": "cmd", "name": "stand_up"})
    a.wait_for("state", timeout=8, predicate=lambda m: m["basic_state"] == 2)
    a.send({"t": "cmd", "name": "torque"})
    a.wait_for("state", timeout=3, predicate=lambda m: m["basic_state"] == 3)
    a.send({"t": "cmd", "name": "step", "value": "on"})
    a.wait_for("state", timeout=4, predicate=lambda m: m["basic_state"] == 4)

    a.send({"t": "cmd", "name": "gait", "value": "stair", "stair_style": "solid",
            "stepping": True})
    res = a.wait_for("gait_result", timeout=8)
    check("楼梯步态未生效时如实报错",
          res.get("ok") is False and res.get("code") == "gait_not_applied",
          res.get("msg", ""))
    check("错误信息指向感知主机", "192.168.1.105" in res.get("msg", ""),
          res.get("msg", ""))

    # 地形图不通不应影响普通步态。
    a.send({"t": "cmd", "name": "gait", "value": "walk"})
    res = a.wait_for("gait_result", timeout=8)
    check("普通步态不受地形图影响", res.get("ok") is True, res.get("msg", ""))
    a.close()


def pose_handoff_scenario(host, port):
    """有官方状态时，旧 2.4G APK 的历史姿态不能污染网关。"""
    print("\n== 姿态交接 ==")
    a = WsClient(host, port)
    a.wait_for("hello")
    st = a.wait_for("state")
    check("交接前网关记的是坐着", st.get("rl_standing") is False, st.get("rl_standing"))

    a.send({"t": "claim", "standing": True})
    a.wait_for("control", predicate=lambda m: m.get("granted") is True)
    st = a.wait_for("state", timeout=5)
    check("官方状态不被 claim.standing 覆盖",
          st.get("body_monitor_alive") is True
          and st.get("rl_standing") is False,
          f"body={st.get('body_monitor_alive')} remembered={st.get('rl_standing')}")

    a.send({"t": "yield"})
    a.send({"t": "claim", "standing": False})
    a.wait_for("control", predicate=lambda m: m.get("granted") is True)
    st = a.wait_for("state", timeout=5)
    check("反向历史姿态同样不覆盖官方状态",
          st.get("posture") == "prone", st.get("posture"))
    a.close()


def arm_steps_scenario(host, port):
    """力控调姿，起步才走。起立后推杆不得当走路。"""
    print("\n== 力控调姿，起步才走 ==")
    a = WsClient(host, port)
    a.wait_for("hello")
    a.send({"t": "claim"})
    a.wait_for("control", predicate=lambda m: m.get("granted") is True)

    a.send({"t": "cmd", "name": "stand_up"})
    st = a.wait_for("state", timeout=8,
                    predicate=lambda m: m.get("basic_state") == 2)
    check("起立后停在初始站立", st.get("basic_state") == 2, st.get("basic_state"))

    deadline = time.time() + 2
    vx = 0.0
    stepping = False
    while time.time() < deadline:
        a.send({"t": "vel", "vx": 0.5, "vy": 0.0, "wz": 0.0})
        try:
            st = a.wait_for("state", timeout=0.2)
            vx = max(vx, st.get("vel", {}).get("x", 0.0))
            if st.get("basic_state") == 4:
                stepping = True
        except TimeoutError:
            pass
    check("起立后推杆不能走", vx < 0.05 and not stepping, f"vx={vx} stepping={stepping}")

    a.send({"t": "cmd", "name": "torque"})
    st = a.wait_for("state", timeout=3,
                    predicate=lambda m: m.get("basic_state") == 3)
    check("力控后进力控站立", st.get("basic_state") == 3, st.get("basic_state"))
    check("力控开放姿态摇杆", st.get("axis_mode") == "pose", st.get("axis_mode"))

    deadline = time.time() + 2
    vx = 0.0
    while time.time() < deadline:
        a.send({"t": "vel", "vx": 0.5, "vy": 0.0, "wz": 0.0})
        try:
            st = a.wait_for("state", timeout=0.2)
            vx = max(vx, st.get("vel", {}).get("x", 0.0))
        except TimeoutError:
            pass
    check("力控后速度轴不能走", vx < 0.05, f"vx={vx}")

    a.send({"t": "cmd", "name": "step", "value": "on"})
    st = a.wait_for("state", timeout=4,
                    predicate=lambda m: m.get("basic_state") == 4)
    check("起步后进踏步", st.get("basic_state") == 4, st.get("basic_state"))
    check("起步开放速度摇杆", st.get("axis_mode") == "vel", st.get("axis_mode"))

    deadline = time.time() + 5
    vx = 0.0
    while time.time() < deadline and vx <= 0.2:
        a.send({"t": "vel", "vx": 0.5, "vy": 0.0, "wz": 0.0})
        try:
            st = a.wait_for("state", timeout=0.2)
            vx = st.get("vel", {}).get("x", 0.0)
        except TimeoutError:
            pass
    check("起步后速度闭环回传", vx > 0.2, f"vx={vx} m/s")

    a.send({"t": "cmd", "name": "step", "value": "off"})
    time.sleep(0.6)
    stopped = a.wait_for(
        "state", timeout=3,
        predicate=lambda m: m.get("motion") == "stopped")
    check("停步后回到姿态控制",
          stopped.get("axis_mode") == "pose",
          stopped.get("axis_mode"))
    deadline = time.time() + 3
    vx = 1.0
    while time.time() < deadline:
        a.send({"t": "vel", "vx": 0.5, "vy": 0.0, "wz": 0.0})
        try:
            st = a.wait_for("state", timeout=0.2)
            vx = min(vx, st.get("vel", {}).get("x", 0.0))
        except TimeoutError:
            pass
    check("停步后速度轴不能走", vx < 0.05, f"vx={vx}")
    a.close()


def media_scenario(host, port):
    """媒体编排：能力协商、码流降级、全码率槽位仲裁。

    这一组不需要真的有视频 —— 网关本来就不搬运视频字节，只下发计划。
    验证的是计划算得对不对，这恰恰是最容易出错也最难在现场调的部分。
    """
    print("\n== 媒体编排 ==")
    a = WsClient(host, port)
    a.wait_for("hello")

    # 连上就该有一份计划，不必等能力上报 —— 否则遥控端要等一个来回才有画面。
    plan = a.wait_for("media_plan", timeout=5)
    check("连接后自动下发媒体计划", plan.get("t") == "media_plan")
    ids = [s["id"] for s in plan["sources"]]
    check("计划包含全部已配置的源", ids == ["ptz_vis", "ptz_ir", "dog_cam"], str(ids))

    # 默认按不支持 H.265 处理，主码流是 H.265 的源应当回退到 H.264 子码流。
    vis = next(s for s in plan["sources"] if s["id"] == "ptz_vis")
    check("默认不假定支持 H.265", vis["codec"] == "h264", vis["codec"])

    # 上报支持 H.265 后，同一个源应当能拿到 H.265 主码流。
    a.send({"t": "media_caps", "h264": True, "h265": True})
    plan = a.wait_for("media_plan", timeout=5)
    a.send({"t": "media_select", "id": "ptz_vis"})
    plan = a.wait_for("media_plan", timeout=5,
                      predicate=lambda m: m.get("main") == "ptz_vis")
    vis = next(s for s in plan["sources"] if s["id"] == "ptz_vis")
    check("上报支持后拿到 H.265 主码流",
          vis["codec"] == "h265" and vis["quality"] == "main",
          f'{vis["codec"]} {vis["quality"]}')
    check("非主视图的源保持缩略图",
          all(s["quality"] == "thumb"
              for s in plan["sources"]
              if s["id"] != "ptz_vis" and s["available"]))

    # 机身相机只有一路 RTSP，没有子码流。它绝不能拿主码流去充当缩略图 ——
    # 那是 2 Mbps，一路就吃掉大半预算，现场表现是"没看几路就卡"。
    # 正确表现是标为不可看，并说明选为主视图才能看。
    dog = next(s for s in plan["sources"] if s["id"] == "dog_cam")
    check("单码流的源不拿主码流冒充缩略图",
          not dog["available"] and "只有一路码流" in dog.get("reason", ""),
          str(dog))
    check("总码率在预算内",
          plan["total_kbps"] <= plan["budget_kbps"] and not plan["over_budget"],
          f'{plan["total_kbps"]}/{plan["budget_kbps"]}')

    # 第二个同样支持 H.265 的客户端选同一个源：全码率只有一份，它必须被挡住。
    b = WsClient(host, port)
    b.wait_for("hello")
    b.wait_for("media_plan", timeout=5)
    b.send({"t": "media_caps", "h264": True, "h265": True})
    b.wait_for("media_plan", timeout=5)
    b.send({"t": "media_select", "id": "ptz_vis"})
    err = b.wait_for("error", timeout=5)
    check("第二个客户端被挡在全码率之外",
          err.get("code") == "media_degraded" and "已有" in err.get("msg", ""),
          err.get("msg", ""))
    bplan = b.wait_for("media_plan", timeout=5)
    bvis = next(s for s in bplan["sources"] if s["id"] == "ptz_vis")
    check("被挡住的客户端仍能看缩略图",
          bvis["available"] and bvis["quality"] == "thumb", str(bvis))

    # 占着全码率槽位的客户端断开后，槽位必须还回去，否则这一路永久废掉。
    a.close()
    time.sleep(0.3)   # 让网关处理完断开
    b.drain()         # A 断开时广播过一份计划，不清掉会读到那份旧的
    b.send({"t": "media_select", "id": "ptz_vis"})
    bplan = b.wait_for("media_plan", timeout=5)
    bvis = next(s for s in bplan["sources"] if s["id"] == "ptz_vis")
    check("前一个客户端断开后槽位被释放", bvis["quality"] == "main", str(bvis))
    b.close()

    # 解不了主码流的客户端不该占着全码率槽位 —— 它退到子码流只吃几百 kbps，
    # 占着就是白白挡住真正用得上的人。
    c = WsClient(host, port)
    c.wait_for("hello")
    c.wait_for("media_plan", timeout=5)
    c.send({"t": "media_caps", "h264": True, "h265": False})
    c.wait_for("media_plan", timeout=5)
    c.send({"t": "media_select", "id": "ptz_vis"})
    err = c.wait_for("error", timeout=5)
    check("不支持 H.265 时如实告知已降级",
          err.get("code") == "media_degraded" and "回退" in err.get("msg", ""),
          err.get("msg", ""))
    cplan = c.wait_for("media_plan", timeout=5)
    cvis = next(s for s in cplan["sources"] if s["id"] == "ptz_vis")
    check("降级后仍可观看（回退到 H.264 子码流）",
          cvis["available"] and cvis["codec"] == "h264" and cvis["downgraded"],
          str(cvis))

    d = WsClient(host, port)
    d.wait_for("hello")
    d.wait_for("media_plan", timeout=5)
    d.send({"t": "media_caps", "h264": True, "h265": True})
    d.wait_for("media_plan", timeout=5)
    d.drain()
    d.send({"t": "media_select", "id": "ptz_vis"})
    dplan = d.wait_for("media_plan", timeout=5,
                       predicate=lambda m: m.get("main") == "ptz_vis")
    dvis = next(s for s in dplan["sources"] if s["id"] == "ptz_vis")
    check("降级的客户端没有占住槽位", dvis["quality"] == "main", str(dvis))
    d.close()

    # 单码流的源被选为主视图时必须能正常看，否则上面那条"不可看"就成了死路。
    c.drain()
    c.send({"t": "media_select", "id": "dog_cam"})
    cplan = c.wait_for("media_plan", timeout=5,
                       predicate=lambda m: m.get("main") == "dog_cam")
    cdog = next(s for s in cplan["sources"] if s["id"] == "dog_cam")
    check("单码流的源选为主视图后可观看",
          cdog["available"] and cdog["quality"] == "main", str(cdog))
    check("切换主视图后总码率仍在预算内",
          not cplan["over_budget"],
          f'{cplan["total_kbps"]}/{cplan["budget_kbps"]}')

    c.send({"t": "media_select", "id": "nonexistent"})
    err = c.wait_for("error", timeout=5)
    check("选择不存在的源被拒绝",
          err.get("code") == "media_degraded" and "没有" in err.get("msg", ""),
          err.get("msg", ""))
    c.close()


def no_media_scenario(host, port):
    """没配媒体源时，控制功能必须完全不受影响。"""
    print("\n== 未配置媒体源 ==")
    a = WsClient(host, port)
    a.wait_for("hello")
    a.send({"t": "media_select", "id": "ptz_vis"})
    err = a.wait_for("error", timeout=5)
    check("媒体消息返回未配置", err.get("code") == "no_media", err.get("msg", ""))

    a.send({"t": "claim"})
    res = a.wait_for("control", predicate=lambda m: "granted" in m)
    check("没有视频不影响申请控制权", res.get("granted") is True)
    a.send({"t": "cmd", "name": "stand"})
    a.wait_for("state", timeout=8, predicate=lambda m: m["basic_state"] == 2)
    check("没有视频不影响状态机指令", True)
    a.close()


def cloud_down_scenario(host, port):
    """感知主机连不上时，点云要如实报错，控制必须照常。

    这是现场最可能遇到的情况：ROS 可达性到今天都没验证过。
    真出问题时，操作员要能从界面上看出是感知主机的事，
    而不是以为整台车坏了。
    """
    print("\n== 感知主机不可达 ==")
    a = WsClient(host, port)
    a.wait_for("hello")

    a.send({"t": "cloud_sub"})
    st = a.wait_for("cloud_status", timeout=5)
    check("订阅被接受", st.get("active") is True, str(st))

    # 连不上时状态里要带出原因，且始终 connected=false
    st = a.wait_for("cloud_status", timeout=6,
                    predicate=lambda m: bool(m.get("error")))
    check("状态里带出感知主机的错误原因",
          "master" in st.get("error", "") or "连不上" in st.get("error", ""),
          st.get("error", ""))
    check("未连通时不谎报已连接", st.get("connected") is False, str(st))

    a.send({"t": "claim"})
    res = a.wait_for("control", predicate=lambda m: "granted" in m)
    check("点云连不上不影响申请控制权", res.get("granted") is True)
    a.send({"t": "cmd", "name": "stand"})
    a.wait_for("state", timeout=8, predicate=lambda m: m["basic_state"] == 2)
    check("点云连不上不影响状态机指令", True)

    a.send({"t": "cloud_unsub"})
    st = a.wait_for("cloud_status", timeout=5,
                    predicate=lambda m: m.get("active") is False)
    check("可以退订", st.get("active") is False, str(st))
    a.close()


def read_conf(path):
    """把 gateway.conf 读成字典。与 deploy/config_util.sh 的 conf_get 同规则。"""
    out = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, val = line.partition("=")
            out[key.strip()] = val.strip()
    return out


def config_scenario(host, port, token, conf_path):
    """在线改配置：令牌、校验、控制权互锁、落盘。

    这一组防的是三件会让人上不了狗的事：谁都能改（协议无身份认证）、
    改成一个本机没有的监听地址（重启后服务起不来，控制台随之消失）、
    以及狗正走着的时候把网关重启掉。
    """
    print("\n== 在线改配置 ==")
    a = WsClient(host, port)
    hello = a.wait_for("hello")
    check("hello 里带出「支持在线改配置」", hello.get("config") is True, str(hello.get("config")))

    # --- 令牌 ---------------------------------------------------------------
    a.send({"t": "config_get"})
    err = a.wait_for("error", timeout=5)
    check("不带令牌读配置被拒", err.get("code") == "bad_admin_token", err.get("msg", ""))

    a.send({"t": "config_get", "token": "definitely-not-it"})
    err = a.wait_for("error", timeout=5)
    check("令牌不对被拒", err.get("code") == "bad_admin_token", err.get("msg", ""))

    # 长度不同和长度相同但内容不同，走的是比较函数里两条不同的分支。
    a.send({"t": "config_get", "token": "0" * len(token)})
    err = a.wait_for("error", timeout=5)
    check("等长但不同的令牌也被拒", err.get("code") == "bad_admin_token", err.get("msg", ""))

    # --- 读回 ---------------------------------------------------------------
    a.send({"t": "config_get", "token": token})
    cfg = a.wait_for("config", timeout=5)
    on_disk = read_conf(conf_path)
    settings = cfg.get("settings", {})
    check("凭令牌读到配置", isinstance(settings, dict) and bool(settings), str(cfg)[:80])
    check("回显的运动主机与文件一致",
          settings.get("robot_ip") == on_disk.get("robot_ip"),
          f'{settings.get("robot_ip")} vs {on_disk.get("robot_ip")}')
    check("回显的感知主机与文件一致",
          settings.get("perception_ip") == on_disk.get("perception_ip"),
          f'{settings.get("perception_ip")} vs {on_disk.get("perception_ip")}')
    check("端口是数字而不是字符串",
          isinstance(settings.get("http_port"), (int, float)),
          type(settings.get("http_port")).__name__)
    check("点云开关是布尔值",
          isinstance(settings.get("cloud_enabled"), bool),
          type(settings.get("cloud_enabled")).__name__)
    # 不是 systemd 托管，网关不该自作主张退出 —— 那样就再也起不来了。
    check("非 systemd 环境下不承诺自动重启",
          cfg.get("auto_restart") is False, str(cfg.get("auto_restart")))

    # --- 校验 ---------------------------------------------------------------
    def reject(label, settings_obj, expect_in=""):
        a.drain()
        a.send({"t": "config_set", "token": token, "settings": settings_obj})
        e = a.wait_for("error", timeout=5)
        ok = e.get("code") == "bad_config"
        if ok and expect_in:
            ok = expect_in in e.get("msg", "")
        check(label, ok, e.get("msg", "")[:70])

    # 这一条是整个功能里最要紧的：填一个本机没有的地址，重启后 bind 失败，
    # 服务起不来，控制台跟着消失，就再没有地方能改回来了。
    reject("监听地址不在本机上被拒", {"bind_address": "203.0.113.9"}, "本机")
    reject("监听地址不是 IP 被拒", {"bind_address": "eth0"})
    reject("运动主机不是合法 IP 被拒", {"robot_ip": "192.168.1"})
    reject("运动主机填 0.0.0.0 被拒", {"robot_ip": "0.0.0.0"})
    # 两台主机填成同一个的话，地形图通道会指向运动主机，上下楼被静默忽略。
    reject("运动与感知主机相同被拒",
           {"robot_ip": "192.168.9.9", "perception_ip": "192.168.9.9"})
    reject("端口超范围被拒", {"http_port": 70000})
    reject("端口给成字符串被拒", {"http_port": "8080"}, "数字")
    reject("IP 给成数字被拒", {"robot_ip": 19216811}, "字符串")
    reject("键名拼错被拒", {"robot_ipp": "192.168.1.9"}, "不认识")
    reject("点云话题不以 / 开头被拒",
           {"cloud_enabled": True, "ros_host": "127.0.0.1",
            "ros_master": "http://127.0.0.1:11311", "cloud_topic": "lidar"})
    reject("ROS master 不带端口被拒",
           {"cloud_enabled": True, "ros_host": "127.0.0.1",
            "ros_master": "http://127.0.0.1"})
    reject("开点云但 ROS 地址不在本机被拒",
           {"cloud_enabled": True, "ros_host": "203.0.113.9",
            "ros_master": "http://127.0.0.1:11311"}, "本机")

    # 校验失败一次都不能落盘，否则重启后就是一份半对的配置。
    check("校验失败没有改动文件", read_conf(conf_path) == on_disk)

    # 反过来，该放行的必须放行。"把 0.0.0.0 收紧成某个具体地址"是文档推荐的
    # 加固动作，而它最容易被自己占着的那个端口误判成"端口已被占用"——
    # 当前监听套接字就在这个端口上，0.0.0.0 和具体地址在同一端口是互斥的。
    a.drain()
    a.send({"t": "config_set", "token": token,
            "settings": {"bind_address": "127.0.0.1"}})
    msg = a.wait_for("config_saved", timeout=5)
    check("只收紧监听地址（端口不变）能通过",
          msg.get("settings", {}).get("bind_address") == "127.0.0.1",
          str(msg.get("settings", {}).get("bind_address")))

    # 换端口时才该真去试探绑定：占着的端口必须被拒。
    import socket as _socket
    squatter = _socket.socket()
    squatter.setsockopt(_socket.SOL_SOCKET, _socket.SO_REUSEADDR, 1)
    squatter.bind(("127.0.0.1", 0))
    squatter.listen(1)
    taken = squatter.getsockname()[1]
    a.drain()
    a.send({"t": "config_set", "token": token, "settings": {"http_port": taken}})
    err = a.wait_for("error", timeout=5)
    check("换到一个已被占用的端口被拒",
          err.get("code") == "bad_config" and "绑不上" in err.get("msg", ""),
          err.get("msg", "")[:70])
    squatter.close()

    on_disk = read_conf(conf_path)   # 上面成功改过一次，基线要跟着更新

    # --- 控制权互锁 ---------------------------------------------------------
    a.drain()
    a.send({"t": "claim"})
    a.wait_for("control", predicate=lambda m: "granted" in m)
    a.send({"t": "config_set", "token": token, "settings": {"local_port": 43896}})
    err = a.wait_for("error", timeout=5)
    check("有人持有控制权时不许改配置",
          err.get("code") == "busy_control", err.get("msg", "")[:70])
    check("被互锁挡下时也没动文件", read_conf(conf_path) == on_disk)

    a.send({"t": "yield"})
    time.sleep(0.2)
    a.drain()

    # --- 真的改一次 ---------------------------------------------------------
    want = {
        "robot_ip": "192.168.1.203",
        "perception_ip": "192.168.1.205",
        "local_port": 43896,
        "cloud_hz": 5,
        "cloud_points": 30000,
    }
    a.send({"t": "config_set", "token": token, "settings": want})
    saved = a.wait_for("config_saved", timeout=5)
    got = saved.get("settings", {})
    check("保存成功并回显新值",
          got.get("robot_ip") == "192.168.1.203" and got.get("local_port") == 43896,
          str(got)[:80])

    disk = read_conf(conf_path)
    check("新值已落盘", disk.get("robot_ip") == "192.168.1.203", disk.get("robot_ip", ""))
    check("落盘的端口也对", disk.get("local_port") == "43896", disk.get("local_port", ""))
    check("落盘的点云参数也对",
          disk.get("cloud_hz") == "5" and disk.get("cloud_points") == "30000",
          f'{disk.get("cloud_hz")} {disk.get("cloud_points")}')
    # 没提到的字段不能被顺手改掉。只发改动项，其余必须原样保留。
    check("没提到的字段保持原样",
          disk.get("http_port") == on_disk.get("http_port") and
          disk.get("bind_address") == on_disk.get("bind_address"),
          f'{disk.get("http_port")} {disk.get("bind_address")}')
    check("写回的文件仍带着说明注释", "#" in open(conf_path, encoding="utf-8").read())

    # 再读一次，网关内存里的那份也该更新了 —— 否则下一次改动会基于旧值算差异。
    a.drain()
    a.send({"t": "config_get", "token": token})
    cfg2 = a.wait_for("config", timeout=5)
    check("再读一次拿到的是新值",
          cfg2["settings"].get("robot_ip") == "192.168.1.203",
          cfg2["settings"].get("robot_ip", ""))

    # 非 systemd 环境下不许自己退出。退了就没人拉它回来。
    time.sleep(1.0)
    a.drain()
    a.send({"t": "config_get", "token": token})
    a.wait_for("config", timeout=5)
    check("保存后网关仍在运行（没有自作主张退出）", True)
    a.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--token", default="54longqr", help="config 场景用的设置密码")
    parser.add_argument("--conf", default="", help="config 场景用的配置文件路径")
    parser.add_argument(
        "--scenario",
        default="full",
        choices=["full", "no-terrain", "media", "no-media", "cloud-down",
                 "pose-handoff", "arm-steps", "config"],
        help="no-terrain 验证感知主机地形图不可达；media/no-media 验证媒体编排；"
             "cloud-down 验证感知主机 ROS 不可达；pose-handoff 验证 2.4G 切回 MESH "
             "时的姿态交接；arm-steps 验证力控调姿、起步才走；config 验证在线改配置",
    )
    args = parser.parse_args()
    host, port = args.host, args.port

    if args.scenario == "config":
        if not args.token or not args.conf:
            print("config 场景需要 --token 与 --conf")
            return 2
        config_scenario(host, port, args.token, args.conf)
        print()
        if FAILURES:
            print(f"失败 {len(FAILURES)} 项: " + ", ".join(FAILURES))
            return 1
        print("全部通过")
        return 0

    standalone = {
        "no-terrain": no_terrain_scenario,
        "media": media_scenario,
        "no-media": no_media_scenario,
        "cloud-down": cloud_down_scenario,
        "pose-handoff": pose_handoff_scenario,
        "arm-steps": arm_steps_scenario,
    }
    if args.scenario in standalone:
        standalone[args.scenario](host, port)
        print()
        if FAILURES:
            print(f"失败 {len(FAILURES)} 项: " + ", ".join(FAILURES))
            return 1
        print("全部通过")
        return 0

    print("\n== HTTP 静态文件 ==")
    status, body = http_get(host, port, "/")
    check("GET / 返回 200", "200" in status, status)
    check("返回的是控制台页面", b"X30" in body, f"{len(body)} 字节")
    status, _ = http_get(host, port, "/nope.js")
    check("不存在的文件返回 404", "404" in status, status)
    status, _ = http_get(host, port, "/../../etc/passwd")
    check("路径穿越被拒绝", "403" in status or "404" in status, status)

    print("\n== 握手与问候 ==")
    a = WsClient(host, port)
    check("Sec-WebSocket-Accept 正确", a.accept_ok)
    hello = a.wait_for("hello")
    check("收到 hello", hello.get("version") is not None, f"版本 {hello.get('version')}")
    check("初始为观察者", hello.get("control") is False)
    client_a = hello["client_id"]

    state = a.wait_for("state")
    check("收到 state 且与运动主机连通", state.get("alive") is True,
          f"状态={state.get('basic_state_text')}")

    print("\n== 控制权仲裁 ==")
    a.send({"t": "vel", "vx": 0.5, "vy": 0, "wz": 0})
    err = a.wait_for("error")
    check("无控制权时速度指令被拒", err.get("code") == "no_control")

    a.send({"t": "claim"})
    grant = a.wait_for("control", predicate=lambda m: "granted" in m)
    check("申请控制权成功", grant.get("granted") is True, f"持有者 #{grant.get('holder')}")

    b = WsClient(host, port)
    b.wait_for("hello")
    b.send({"t": "claim"})
    deny = b.wait_for("control", predicate=lambda m: "granted" in m)
    check("第二客户端被拒绝", deny.get("granted") is False, f"持有者 #{deny.get('holder')}")

    print("\n== 步态前置条件 ==")
    # 还坐着，楼梯步态应当被编排器挡下并给出可读原因，而不是发出去石沉大海。
    a.send({"t": "cmd", "name": "gait", "value": "stair", "stair_style": "solid"})
    res = a.wait_for("gait_result", timeout=5)
    check("坐姿下切楼梯被拒绝",
          res.get("ok") is False and res.get("code") == "not_standing",
          res.get("msg", ""))

    print("\n== 状态机 ==")
    a.send({"t": "cmd", "name": "stand"})
    st = a.wait_for("state", timeout=8, predicate=lambda m: m["basic_state"] == 2)
    check("坐 -> 初始站立", st["basic_state"] == 2, st["basic_state_text"])
    check("规范状态为停步",
          st.get("posture") == "standing" and st.get("motion") == "stopped",
          f"{st.get('posture')}/{st.get('motion')}")
    st = a.wait_for("state", timeout=3,
                    predicate=lambda m: m.get("body_monitor_alive") is True)
    check("官方本体监控作为 UDP 补充状态源",
          st.get("state_truth") == "motion_udp"
          and st.get("body_motion_state") == 2,
          f"{st.get('state_truth')} state={st.get('body_motion_state')}")

    print("\n== 步态与速度 ==")
    # 力控/停步时只记配置，不应提前发给狗主机。
    a.send({"t": "cmd", "name": "gait", "value": "offroad"})
    res = a.wait_for("gait_result", timeout=5)
    check("停步时记下越野步态",
          res.get("ok") is True and res.get("code") == "queued", res.get("msg", ""))
    a.send({"t": "cmd", "name": "height", "value": "crawl"})
    a.send({"t": "cmd", "name": "step", "value": "on"})
    st = a.wait_for("state", timeout=6,
                    predicate=lambda m: m["gait_key"] == "offroad"
                    and m.get("height_gear") == -1
                    and m.get("motion") == "walking")
    check("起步确认后执行越野与身高", st["gait_key"] == "offroad",
          f"{st['gait_text']}，速度上限 {st['limits']['forward']} m/s")

    # 平板会以 20 Hz 持续喂，这里如实模拟；只发一次是喂不动看门狗的。
    limit = st["limits"]["forward"]
    deadline = time.time() + 3.0
    while time.time() < deadline:
        a.send({"t": "vel", "vx": 1.0, "vy": 0.0, "wz": 0.0})
        time.sleep(0.05)
    a.drain()
    st = a.wait_for("state")
    check("机体前向速度已建立", st["vel"]["x"] > limit * 0.5,
          f"vx={st['vel']['x']} / 上限 {limit} m/s")
    check("里程计已推进", abs(st["odom"]["x"]) > 0.05, f"x={st['odom']['x']} m")

    print("\n== 看门狗 ==")
    # 停发指令，网关的看门狗应当把轴指令清零，机器人随之减速停下。
    time.sleep(2.0)
    a.drain()
    st = a.wait_for("state")
    check("停发后速度归零", abs(st["vel"]["x"]) < 0.05, f"vx={st['vel']['x']} m/s")

    a.send({"t": "cmd", "name": "step", "value": "off"})
    st = a.wait_for("state", timeout=5, predicate=lambda m: m.get("motion") == "stopped")
    check("行走状态可回到停步", st.get("motion") == "stopped", st.get("motion"))

    a.send({"t": "cmd", "name": "torque"})
    st = a.wait_for("state", timeout=8, predicate=lambda m: m["basic_state"] == 3)
    check("初始站立 -> 力控站立", st["basic_state"] == 3, st["basic_state_text"])

    a.send({"t": "cmd", "name": "step"})
    err = a.wait_for("error", timeout=3)
    check("起停必须明确指定 on/off", err.get("code") == "bad_request", err.get("msg", ""))

    print("\n== 上下楼 ==")
    # 单帧楼梯：编排器要先把地形图设成实心踏面，再切步态。
    # 仿真器在地形图模式不匹配时会静默忽略步态指令，正好复现实机行为，
    # 所以这一项通过就说明两条 UDP 通道的配合是对的。
    a.send({"t": "cmd", "name": "gait", "value": "stair", "stair_style": "solid"})
    res = a.wait_for("gait_result", timeout=8)
    check("力控时记下楼梯步态", res.get("code") == "queued", res.get("msg", ""))
    a.send({"t": "cmd", "name": "height", "value": "normal"})
    a.send({"t": "cmd", "name": "step", "value": "on"})
    applied = a.wait_for("gait_result", timeout=8,
                         predicate=lambda m: m.get("code") != "queued")
    check("起步后执行楼梯配置", applied.get("ok") is True, applied.get("msg", ""))
    st = a.wait_for("state", timeout=5, predicate=lambda m: m["gait_key"] == "stair")
    check("遥测确认步态已生效", st["gait_key"] == "stair", st["gait_text"])

    # 多帧楼梯：额外要求静止，且地形图要走"准备 -> 多帧"两步。
    a.send({"t": "cmd", "name": "gait", "value": "stairmulti"})
    res = a.wait_for("gait_result", timeout=8)
    check("切换到多帧楼梯步态", res.get("ok") is True, res.get("msg", ""))
    a.drain()
    st = a.wait_for("state", timeout=5,
                    predicate=lambda m: m["gait_key"] == "stairmulti")
    check("遥测确认多帧步态已生效", st["gait_key"] == "stairmulti", st["gait_text"])

    # 退出楼梯回到 Walk，顺带验证障碍高度阈值会被回设。
    a.send({"t": "cmd", "name": "gait", "value": "walk"})
    res = a.wait_for("gait_result", timeout=8)
    check("退出楼梯回到 Walk", res.get("ok") is True, res.get("msg", ""))

    print("\n== 急停免鉴权 ==")
    # b 没有控制权，但急停必须放行。
    b.send({"t": "cmd", "name": "estop"})
    st = b.wait_for("state", timeout=5,
                    predicate=lambda m: m["basic_state"] == 6 or m["emergency_source"] != 0)
    check("观察者也能触发急停",
          st["basic_state"] == 6 or st["emergency_source"] != 0,
          f"状态={st['basic_state_text']} 急停源={st['emergency_source']}")
    st = b.wait_for("state", timeout=3,
                    predicate=lambda m: m.get("body_motion_state") == 6)
    check("官方本体监控识别软急停",
          st.get("posture") == "locked" and st.get("axis_mode") == "none",
          f"{st.get('posture')} / {st.get('axis_mode')}")

    print("\n== 断连释放控制权 ==")
    a.close()
    time.sleep(0.5)
    b.send({"t": "claim"})
    grant = b.wait_for("control", timeout=5, predicate=lambda m: "granted" in m)
    check("持有者断开后控制权可被接管", grant.get("granted") is True)
    b.close()

    print()
    if FAILURES:
        print(f"失败 {len(FAILURES)} 项: " + ", ".join(FAILURES))
        return 1
    print("全部通过")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # noqa: BLE001
        print(f"\n探针异常: {type(exc).__name__}: {exc}")
        sys.exit(2)
