#!/usr/bin/env python3
"""绝影 X30 运动主机仿真器。

在没有实机的情况下验证网关的协议实现是否正确。仿真器会：
  - 在 43893 上收指令，解析并复现机器人的基础状态机；
  - 按真实频率向登记的目标回送 0x1008 / 0x1009 / 0x100A / 0x100B / 0x21050F0A
    五种遥测报文，结构体布局与 API 文档一致；
  - 复现协议里的关键约束 —— 心跳超时判定、轴指令 1 秒失效、趴着时忽略起步、
    非踏步态忽略步态切换。

刻意不做的事：不模拟动力学。里程计只是把速度指令做一阶积分，用于验证数据通路，
不能用来评估控制效果。

用法:
    python tools/x30_sim.py [--listen-port 43893] [--target 127.0.0.1:43897]
"""

import argparse
import math
import socket
import struct
import sys
import threading
import time

# --- 指令码 ----------------------------------------------------------------

HEARTBEAT = 0x21040001
CONNECT_CONFIRM = 0x21020001
STAND_SIT = 0x21010202
TORQUE_STAND = 0x2101020A
STEPPING = 0x21010201
MODE_NON_MANUAL = 0x21010C03
MODE_MANUAL = 0x21010C02
BODY_HEIGHT = 0x21010406
SOFT_ESTOP = 0x21010C0E
SAVE_DATA = 0x010C01
SAVE_DATA_LEGACY = 18

AXIS_LEFT_Y = 0x21010130
AXIS_LEFT_X = 0x21010131
AXIS_RIGHT_X = 0x21010135
AXIS_RIGHT_Y = 0x21010102
AXIS_CODES = {AXIS_LEFT_Y, AXIS_LEFT_X, AXIS_RIGHT_X, AXIS_RIGHT_Y}

GAITS = {
    0x21010300: (0, "Walk"),
    0x21010402: (2, "缓坡"),
    0x21010401: (1, "越野"),
    0x21010405: (6, "楼梯"),
    0x2101040A: (7, "楼梯(多帧)"),
    0x2101040B: (8, "45°楼梯(多帧)"),
    0x21010420: (32, "L-Walk"),
    0x21010421: (33, "山地"),
    0x21010422: (34, "静音"),
}

# --- 地形图模块（感知主机 192.168.1.105:43899）------------------------------

HEIGHT_MAP_MODE = 0x3101EE01
BRAKE_MODE = 0x3101EE02
VEL_SOURCE = 0x3101EE03
STEP_Z_MAX = 0x3100EE04

MAP_SOLID = 3
MAP_GRATING = 4
MAP_NO_RISER = 5
MAP_MULTI_PREP = 18
MAP_MULTI = 20

MAP_NAMES = {
    MAP_SOLID: "实心踏面",
    MAP_GRATING: "格栅踏面",
    MAP_NO_RISER: "无踢面",
    MAP_MULTI_PREP: "多帧准备",
    MAP_MULTI: "多帧",
}

# 每种楼梯步态可接受的地形图模式。这是文档「三种楼梯步态必须配合对应的
# 地形图模式，否则不生效」的具体化，也是本仿真器最有价值的一条行为：
# 不匹配时静默忽略，正好复现实机上最难查的那种失败。
STAIR_REQUIREMENTS = {
    6: {MAP_SOLID, MAP_GRATING, MAP_NO_RISER},
    7: {MAP_MULTI},
    8: {MAP_MULTI},
}

# --- 遥测报文码 -------------------------------------------------------------

TELEM_RUNNING = 0x1008
TELEM_MOTION = 0x1009
TELEM_SENSOR = 0x100A
TELEM_SAFE = 0x100B
TELEM_BATTERY = 0x21050F0A
TELEM_HEIGHT = 0x11050F08

# --- 基础状态 ---------------------------------------------------------------

SITTING = 0
SIT_TO_STAND = 1
INITIAL_STANDING = 2
TORQUE_STANDING = 3
STEPPING_STATE = 4
STAND_TO_SIT = 5
EMERGENCY = 6

STATE_NAMES = {
    SITTING: "坐下",
    SIT_TO_STAND: "起立中",
    INITIAL_STANDING: "初始站立",
    TORQUE_STANDING: "力控站立",
    STEPPING_STATE: "踏步",
    STAND_TO_SIT: "坐下中",
    EMERGENCY: "急停/跌倒",
}

AXIS_MAX = 32767
AXIS_DEAD_ZONE = 655

# 各步态最大速度，取自 API 文档附录 B（正常身高档）
GAIT_LIMITS = {
    0: (1.2, 0.8, 1.2),
    1: (0.3, 0.1, 0.45),
    2: (0.7, 0.3, 0.7),
    3: (2.5, 0.8, 1.2),
    6: (0.3, 0.1, 0.45),
    7: (0.6, 0.1, 0.45),
    8: (0.3, 0.1, 0.45),
    32: (1.0, 0.5, 1.0),
    33: (1.0, 0.5, 1.0),
    34: (1.0, 0.5, 1.0),
}

HEAD = struct.Struct("<III")


def head(code, value=0, msg_type=0):
    return HEAD.pack(code & 0xFFFFFFFF, value & 0xFFFFFFFF, msg_type)


def complex_msg(code, payload):
    return HEAD.pack(code & 0xFFFFFFFF, len(payload), 1) + payload


def axis_to_value(raw, limit):
    """复现文档里的轴值到物理量的映射，含死区。"""
    if abs(raw) < AXIS_DEAD_ZONE:
        return 0.0
    return raw / AXIS_MAX * limit


class Robot:
    """基础状态机与运动学积分。"""

    def __init__(self):
        self.lock = threading.Lock()
        self.state = SITTING
        self.gait = 0
        self.height_gear = 0  # 0 = 正常, -1 = 匍匐
        self.nav_mode = 0
        self.emergency_source = 0
        self.axes = {c: 0 for c in AXIS_CODES}
        self.axis_stamp = 0.0
        self.last_heartbeat = 0.0
        self.connected = False
        # 收包线程每转一圈就刷新它（含空转），用来判断"没收到心跳"到底是
        # 对方真没发，还是本进程自己没排上队。详见 step() 里的说明。
        self.rx_tick = 0.0

        self.x = 0.0
        self.y = 0.0
        self.yaw = 0.0
        self.vx = 0.0
        self.vy = 0.0
        self.wz = 0.0
        self.mileage_cm = 0.0
        self.boot_time = time.time()
        self.transition_at = None
        self.transition_to = None

        # 地形图模块状态。实机上这些在感知主机里，仿真器为了能验证
        # 「步态 + 地形图」的配合关系，把它们放在同一个对象里。
        self.height_map_mode = None
        self.step_z_max = 1  # 1 = 8cm, 2 = 28cm

    # -- 指令处理 ------------------------------------------------------------

    def handle(self, code, value, log):
        now = time.time()

        if code == HEARTBEAT:
            with self.lock:
                self.last_heartbeat = now
            return
        if code == CONNECT_CONFIRM:
            with self.lock:
                self.connected = True
            log("收到连接确认")
            return

        if code in AXIS_CODES:
            signed = struct.unpack("<i", struct.pack("<I", value))[0]
            with self.lock:
                self.axes[code] = signed
                self.axis_stamp = now
            return

        with self.lock:
            if code == SOFT_ESTOP:
                self.state = EMERGENCY
                self.emergency_source = 5  # 客户端触发
                self._zero_axes()
                log("软急停 -> 趴下并锁关节")

            elif code == STAND_SIT:
                if self.state == SITTING:
                    self._begin(SIT_TO_STAND, INITIAL_STANDING, 2.0, log, "起立")
                elif self.state in (INITIAL_STANDING, TORQUE_STANDING):
                    self._begin(STAND_TO_SIT, SITTING, 2.0, log, "坐下")
                elif self.state == EMERGENCY:
                    # 手册里跌倒后可以尝试直接站起来
                    self._begin(SIT_TO_STAND, INITIAL_STANDING, 2.0, log, "从急停恢复")
                    self.emergency_source = 0
                else:
                    log(f"忽略站坐指令，当前是「{STATE_NAMES[self.state]}」")

            elif code == TORQUE_STAND:
                if self.state == INITIAL_STANDING:
                    self.state = TORQUE_STANDING
                    log("进入力控站立")
                else:
                    log(f"忽略力控指令，当前是「{STATE_NAMES[self.state]}」")

            elif code == STEPPING:
                if self.state == TORQUE_STANDING:
                    self.state = STEPPING_STATE
                    log("开始踏步")
                elif self.state == STEPPING_STATE:
                    self.state = TORQUE_STANDING
                    self._zero_axes()
                    log("停止踏步")
                else:
                    # 文档明确说明：趴着时发起步不生效
                    log(f"忽略起步指令，当前是「{STATE_NAMES[self.state]}」")

            elif code in GAITS:
                gait_id, name = GAITS[code]
                required = STAIR_REQUIREMENTS.get(gait_id)
                if self.state != STEPPING_STATE:
                    log(f"忽略步态切换，只有踏步态才能切步态（当前「{STATE_NAMES[self.state]}」）")
                elif self.height_gear == -1 and gait_id not in (0, 2):
                    log("忽略步态切换，匍匐档只支持 Walk 与缓坡")
                elif required is not None and self.height_map_mode not in required:
                    # 实机在这里也是静默忽略的，不回任何错误报文。
                    want = "/".join(MAP_NAMES[m] for m in sorted(required))
                    got = MAP_NAMES.get(self.height_map_mode, "未设置")
                    log(f"忽略「{name}」，地形图模式需为 {want}，当前 {got}")
                else:
                    self.gait = gait_id
                    log(f"切换步态 -> {name}")

            elif code == BODY_HEIGHT:
                if value == 0:
                    self.height_gear = -1
                    log("身高 -> 匍匐")
                elif value == 2:
                    self.height_gear = 0
                    log("身高 -> 正常")

            elif code == MODE_MANUAL:
                self.nav_mode = 0
                log("控制模式 -> 手动")
            elif code == MODE_NON_MANUAL:
                self.nav_mode = 1
                log("控制模式 -> 非手动")

            elif code in (SAVE_DATA, SAVE_DATA_LEGACY):
                log("保存数据并退出运动程序（仿真器仅记录，不退出）")

            else:
                log(f"未识别的指令码 0x{code:08X} 值={value}")

    def handle_terrain(self, code, value, log):
        """感知主机 43899 上的地形图指令。只写通道，不回任何报文。"""
        with self.lock:
            if code == HEIGHT_MAP_MODE:
                if value not in MAP_NAMES:
                    log(f"[地形图] 未知模式值 {value}")
                    return
                # 多帧模式只能在静止时切换，运动中收到就忽略。
                moving = max(abs(self.vx), abs(self.vy), abs(self.wz)) > 0.05
                if value in (MAP_MULTI_PREP, MAP_MULTI) and moving:
                    log(f"[地形图] 忽略「{MAP_NAMES[value]}」，多帧模式只能在静止时切换")
                    return
                self.height_map_mode = value
                log(f"[地形图] 模式 -> {MAP_NAMES[value]}")
            elif code == STEP_Z_MAX:
                self.step_z_max = value
                log(f"[地形图] 障碍高度阈值 -> {'28cm' if value == 2 else '8cm'}")
            elif code == VEL_SOURCE:
                log(f"[地形图] 速度输入源 -> {'导航' if value == 2 else '手柄'}")
            elif code == BRAKE_MODE:
                log(f"[地形图] 避障策略 -> {'绕行' if value == 2 else '减速'}")
            else:
                log(f"[地形图] 未识别的指令码 0x{code:08X} 值={value}")

    def _begin(self, transient, target, seconds, log, what):
        self.state = transient
        self.transition_to = target
        self.transition_at = time.time() + seconds
        log(f"{what}中…")

    def _zero_axes(self):
        for c in self.axes:
            self.axes[c] = 0

    # -- 积分 ----------------------------------------------------------------

    def step(self, dt, log):
        now = time.time()
        with self.lock:
            if self.transition_at is not None and now >= self.transition_at:
                self.state = self.transition_to
                self.transition_at = None
                self.transition_to = None
                log(f"进入「{STATE_NAMES[self.state]}」")

            # 心跳超时：文档说运动程序会判定断连。
            #
            # 但下判断之前得先确认**本仿真器自己的收包线程还在转**。
            # tx_loop 是 200 Hz、每拍三个包，在 CPython 里它几乎一直握着 GIL；
            # 开发机上同时在编译或打包时，rx_loop 可能一秒多拿不到 GIL ——
            # 心跳其实早就躺在 socket 缓冲区里了，只是没人取。
            # 这时候报"断连"是在冤枉网关：一整片断言会莫名其妙地红，
            # 单独重跑却全是好的，属于最难查也最败坏信任的那种失败。
            #
            # rx_loop 即便没有流量也会因 0.2s 超时而空转，所以对方真的
            # 停发心跳时 rx_tick 照样是新的，这一条协议约束仍然测得到。
            rx_healthy = now - self.rx_tick < 0.5
            if self.connected and now - self.last_heartbeat > 1.0 and rx_healthy:
                self.connected = False
                self._zero_axes()
                log("心跳超时，判定断连")

            # 轴指令超过 1 秒未刷新即失效
            axes_valid = (now - self.axis_stamp) <= 1.0
            if not axes_valid:
                self._zero_axes()

            if self.state == STEPPING_STATE:
                fwd, lat, yaw_rate = GAIT_LIMITS.get(self.gait, (1.2, 0.8, 1.2))
                if self.height_gear == -1 and self.gait in (0, 2):
                    fwd *= 0.5  # 匍匐档前向速度减半
                target_vx = axis_to_value(self.axes[AXIS_LEFT_Y], fwd)
                # Y 向与偏航在协议里带负号
                target_vy = -axis_to_value(self.axes[AXIS_LEFT_X], lat)
                target_wz = -axis_to_value(self.axes[AXIS_RIGHT_X], yaw_rate)
            else:
                target_vx = target_vy = target_wz = 0.0

            # 一阶滞后，粗略模拟加减速
            alpha = min(1.0, dt * 3.0)
            self.vx += (target_vx - self.vx) * alpha
            self.vy += (target_vy - self.vy) * alpha
            self.wz += (target_wz - self.wz) * alpha

            self.yaw += self.wz * dt
            self.x += (self.vx * math.cos(self.yaw) - self.vy * math.sin(self.yaw)) * dt
            self.y += (self.vx * math.sin(self.yaw) + self.vy * math.cos(self.yaw)) * dt
            self.mileage_cm += math.hypot(self.vx, self.vy) * dt * 100.0

    # -- 遥测打包 ------------------------------------------------------------

    def pack_running(self):
        with self.lock:
            error_state = 0
            if not self.connected:
                error_state |= 1 << 1  # wifi_error = 心跳超时
            run_s = int(time.time() - self.boot_time)
            payload = struct.pack(
                "<15sxii4q4f",
                b"X30-SIM",
                int(self.mileage_cm),
                int(self.mileage_cm) + 123456,
                run_s,
                run_s + 98765,
                run_s,
                run_s + 54321,
                self.axes[AXIS_LEFT_X] / AXIS_MAX,
                self.axes[AXIS_LEFT_Y] / AXIS_MAX,
                self.axes[AXIS_RIGHT_X] / AXIS_MAX,
                self.axes[AXIS_RIGHT_Y] / AXIS_MAX,
            )
            payload += struct.pack("<BB8x", self.nav_mode, self.emergency_source)
            payload += struct.pack("<2xI", error_state)
        assert len(payload) == 88, len(payload)
        return complex_msg(TELEM_RUNNING, payload)

    def pack_motion(self):
        with self.lock:
            payload = struct.pack(
                "<BB2xff3f3ffI I 10s2x",
                self.state,
                self.gait,
                0.0,
                0.0,
                self.x,
                self.y,
                self.yaw,
                self.vx,
                self.vy,
                self.wz,
                self.mileage_cm,
                0,
                0,
                b"\x00" * 10,
            )
        assert len(payload) == 60, len(payload)
        return complex_msg(TELEM_MOTION, payload)

    def pack_sensor(self):
        with self.lock:
            # 姿态角单位是度；仿真里只让 yaw 跟着积分走，roll/pitch 给点小抖动
            t = time.time()
            imu = struct.pack(
                "<i9f",
                int(t * 1000) & 0x7FFFFFFF,
                math.sin(t * 1.3) * 1.5,
                math.sin(t * 0.9) * 1.2,
                math.degrees(self.yaw),
                self.wz * 0.0,
                0.0,
                self.wz,
                0.0,
                0.0,
                9.81,
            )
            # 12 个关节给一组静态的合理值，够验证解析即可
            pos = struct.pack("<12f", *([0.0, 0.8, -1.6] * 4))
            vel = struct.pack("<12f", *([0.0] * 12))
            tau = struct.pack("<12f", *([0.0, 12.0, -8.0] * 4))
        payload = imu + pos + vel + tau
        assert len(payload) == 184, len(payload)
        return complex_msg(TELEM_SENSOR, payload)

    def pack_safe(self):
        base = 38.0 + math.sin(time.time() * 0.2) * 2.0
        payload = struct.pack("<12f", *[base + i * 0.4 for i in range(12)])
        payload += struct.pack("<12B", *[int(base) + i for i in range(12)])
        payload += struct.pack("<ff", 52.0, 2200.0)
        assert len(payload) == 68, len(payload)
        return complex_msg(TELEM_SAFE, payload)

    def pack_battery(self):
        payload = struct.pack(
            "<HhHHHHHHHBBBBB x 4f",
            72,      # voltage
            -350,    # current, 10mA
            18000,   # remaining capacity
            22400,   # nominal capacity
            42,      # cycles
            0x3181,  # production date，电池 BMS 的压缩日期格式，仅占位
            0, 0, 0,
            3,       # software version
            80,      # battery level %
            1, 1, 25,
            28.0, 28.5, 29.0, 28.2,
        )
        assert len(payload) == 40, len(payload)
        return complex_msg(TELEM_BATTERY, payload)

    def pack_height(self):
        raw = struct.unpack("<I", struct.pack("<i", self.height_gear))[0]
        return head(TELEM_HEIGHT, raw)


def main():
    parser = argparse.ArgumentParser(description="绝影 X30 运动主机仿真器")
    parser.add_argument("--listen-port", type=int, default=43893)
    parser.add_argument(
        "--terrain-port",
        type=int,
        default=43899,
        help="地形图模块端口，实机上在感知主机 192.168.1.105",
    )
    parser.add_argument(
        "--target",
        default="127.0.0.1:43897",
        help="遥测回送目标，对应实机 network.toml 里登记的地址",
    )
    parser.add_argument("--quiet", action="store_true", help="只打印状态迁移")
    args = parser.parse_args()

    host, _, port = args.target.rpartition(":")
    target = (host or "127.0.0.1", int(port))

    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    rx.bind(("0.0.0.0", args.listen_port))
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    terrain_rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    terrain_rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    terrain_rx.bind(("0.0.0.0", args.terrain_port))

    robot = Robot()
    stop = threading.Event()

    def log(msg):
        print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)

    log(f"仿真器已启动，监听 {args.listen_port}，遥测回送至 {target[0]}:{target[1]}")
    log(f"地形图模块监听 {args.terrain_port}")
    log("等待网关连接…")

    def rx_loop():
        rx.settimeout(0.2)
        while not stop.is_set():
            # 每转一圈都打个卡，空转也算。step() 用它区分"对方没发心跳"
            # 和"本线程没排上队"。
            robot.rx_tick = time.time()
            try:
                data, _ = rx.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if len(data) < HEAD.size:
                continue
            code, value, _msg_type = HEAD.unpack_from(data, 0)
            robot.handle(code, value, log if not args.quiet else lambda _m: None)

    def terrain_loop():
        terrain_rx.settimeout(0.2)
        while not stop.is_set():
            try:
                data, _ = terrain_rx.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if len(data) < HEAD.size:
                continue
            code, value, _msg_type = HEAD.unpack_from(data, 0)
            robot.handle_terrain(
                code, value, log if not args.quiet else lambda _m: None
            )

    def tx_loop():
        # 各报文按文档给定的频率下发
        period = 1.0 / 200.0
        tick = 0
        next_t = time.perf_counter()
        while not stop.is_set():
            robot.step(period, log)
            try:
                tx.sendto(robot.pack_running(), target)
                tx.sendto(robot.pack_motion(), target)
                tx.sendto(robot.pack_sensor(), target)
                if tick % 200 == 0:  # 1 Hz
                    tx.sendto(robot.pack_safe(), target)
                    tx.sendto(robot.pack_height(), target)
                if tick % 400 == 0:  # 0.5 Hz
                    tx.sendto(robot.pack_battery(), target)
            except OSError:
                pass
            tick += 1
            next_t += period
            delay = next_t - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
            else:
                next_t = time.perf_counter()

    threads = [
        threading.Thread(target=rx_loop, daemon=True),
        threading.Thread(target=terrain_loop, daemon=True),
        threading.Thread(target=tx_loop, daemon=True),
    ]
    for t in threads:
        t.start()

    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        log("停止")
        stop.set()
        rx.close()
        tx.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
