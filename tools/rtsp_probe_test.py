#!/usr/bin/env python3
"""rtsp_probe 的自测：SPS 解析对不对，以及能不能真的探一路 RTSP。

SPS 解析特别值得测：指数哥伦布码和去竞争字节很容易写错，而错了不会报异常，
只会安静地给出一个离谱的分辨率。现场看到"3200x1800"多半只会以为相机怪，
不会怀疑到这段代码上。
"""

import base64
import importlib.util
import socket
import subprocess
import sys
import threading
import time

spec = importlib.util.spec_from_file_location(
    "rtsp_probe", __file__.replace("rtsp_probe_test.py", "rtsp_probe.py"))
rp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rp)

PASS = 0
FAIL = 0


def check(name, ok, detail=""):
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"  [OK]   {name}")
    else:
        FAIL += 1
        print(f"  [FAIL] {name} {detail}")


# --- SPS 解析 ---------------------------------------------------------------
#
# 不靠"记得的"样本来测。凭印象抄一段 base64 很容易抄错，而错了的表现是
# 解析结果离谱 —— 到时候分不清是解析器的锅还是样本的锅，白白浪费时间。
# 这里反过来：按 ITU-T H.264 §7.3.2.1 自己**造**一段 SPS，再看能不能解回去。

class BitWriter:
    def __init__(self):
        self.bits = []

    def u(self, n, v):
        for i in range(n - 1, -1, -1):
            self.bits.append((v >> i) & 1)

    def ue(self, v):
        v += 1
        n = v.bit_length()
        self.u(n - 1, 0)
        self.u(n, v)

    def bytes(self, nal_header):
        while len(self.bits) % 8:
            self.bits.append(0)
        raw = bytearray()
        for i in range(0, len(self.bits), 8):
            b = 0
            for bit in self.bits[i:i + 8]:
                b = (b << 1) | bit
            raw.append(b)

        # 插入防竞争字节，顺带验证解析器的去竞争逻辑
        out = bytearray([nal_header])
        zeros = 0
        for b in raw:
            if zeros == 2 and b <= 3:
                out.append(3)
                zeros = 0
            out.append(b)
            zeros = zeros + 1 if b == 0 else 0
        return bytes(out)


def make_sps(width, height, profile=100):
    """造一段能解出指定宽高的 SPS。高度不是 16 的倍数时用裁剪补齐，
    这正是 1080p 的真实情形（1080 / 16 = 67.5）。"""
    w_mbs = (width + 15) // 16
    h_mbs = (height + 15) // 16
    crop_r = (w_mbs * 16 - width) // 2      # chroma 4:2:0，SubWidthC=2
    crop_b = (h_mbs * 16 - height) // 2     # frame_mbs_only=1，SubHeightC=2

    w = BitWriter()
    w.u(8, profile)
    w.u(8, 0)          # constraint flags
    w.u(8, 41)         # level_idc
    w.ue(0)            # sps_id
    if profile in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139,
                   134, 135):
        w.ue(1)        # chroma_format_idc = 4:2:0
        w.ue(0)        # bit_depth_luma_minus8
        w.ue(0)        # bit_depth_chroma_minus8
        w.u(1, 0)      # qpprime_y_zero_transform_bypass
        w.u(1, 0)      # seq_scaling_matrix_present
    w.ue(0)            # log2_max_frame_num_minus4
    w.ue(2)            # pic_order_cnt_type = 2
    w.ue(1)            # max_num_ref_frames
    w.u(1, 0)          # gaps_in_frame_num_value_allowed
    w.ue(w_mbs - 1)
    w.ue(h_mbs - 1)
    w.u(1, 1)          # frame_mbs_only_flag
    w.u(1, 1)          # direct_8x8_inference_flag
    if crop_r or crop_b:
        w.u(1, 1)
        w.ue(0); w.ue(crop_r); w.ue(0); w.ue(crop_b)
    else:
        w.u(1, 0)
    w.u(1, 0)          # vui_parameters_present
    w.u(1, 1)          # rbsp_stop_one_bit
    return w.bytes(0x67)


print("\n== H.264 SPS 分辨率解析（自造样本，覆盖裁剪路径）==")
for prof in (66, 100):
    tag = "Baseline" if prof == 66 else "High"
    for w_, h_ in [(1920, 1080), (1280, 720), (640, 480), (352, 288),
                   (704, 576), (2592, 1944)]:
        try:
            got = rp.h264_resolution(make_sps(w_, h_, prof))
        except Exception as e:  # noqa: BLE001
            got = f"异常 {e}"
        check(f"{tag} {w_}x{h_}", got == (w_, h_), f"实得 {got}")

# 再用两段真实相机出的 SPS 兜一下。宽高是解析出来后人工核对过的：
# 字段全部自洽（sps_id=0、位深 0、宏块数整除），bit 对不齐的话会像
# 随手编的样本那样解出荒唐的裁剪值。
print("\n== 真实相机 SPS ==")
for b64, w_, h_ in [
    ("Z2QAKawspADwAQ+wFQAAAwAEAAADAPA8YMZY", 3840, 2160),
    ("Z2QAH6zZQFAFuwFQAAADABAAAAMDyPFCmA==", 1280, 720),
]:
    got = rp.h264_resolution(base64.b64decode(b64 + "=="))
    check(f"{w_}x{h_}（含防竞争字节）", got == (w_, h_), f"实得 {got}")

# 坏数据不能抛异常，只能返回 None —— 分辨率是锦上添花，
# 不该让整个探测因为它失败。
print("\n== 坏数据要能兜住 ==")
for bad in [b"", b"\x67", b"\x67\xff\xff\xff", b"\x00" * 40]:
    try:
        got = rp.h264_resolution(bad)
        check(f"{len(bad)} 字节坏数据返回 None 或结果而不抛异常", True)
    except Exception as e:  # noqa: BLE001
        check(f"{len(bad)} 字节坏数据", False, f"抛了 {type(e).__name__}: {e}")


# --- SDP 解析 ---------------------------------------------------------------
print("\n== SDP 解析 ==")
SDP = """v=0
o=- 0 0 IN IP4 127.0.0.1
s=Session
m=video 0 RTP/AVP 96
a=rtpmap:96 H264/90000
a=fmtp:96 packetization-mode=1; sprop-parameter-sets=Z2QAKawspADwAQ+wFQAAAwAEAAADAPA8YMZY,aOvssiw=
m=audio 0 RTP/AVP 8
a=rtpmap:8 PCMA/8000
"""
tracks = rp.parse_sdp(SDP)
check("识别出两条轨", len(tracks) == 2, str(tracks))
check("视频编码是 H264",
      any(t["kind"] == "video" and t["codec"] == "H264" for t in tracks))
check("音频编码是 PCMA",
      any(t["kind"] == "audio" and t["codec"] == "PCMA" for t in tracks))

no_audio = rp.parse_sdp("m=video 0 RTP/AVP 96\na=rtpmap:96 H265/90000\n")
check("只有视频轨时不误报音频",
      len(no_audio) == 1 and no_audio[0]["codec"] == "H265", str(no_audio))


# --- URL 解析 ---------------------------------------------------------------
print("\n== URL 解析 ==")
host, port, user, pw, clean = rp.parse_url("rtsp://192.168.1.105:8554/test")
check("无认证地址", (host, port, user, pw) == ("192.168.1.105", 8554, None, None),
      f"{host} {port} {user} {pw}")
check("请求行保留端口", clean == "rtsp://192.168.1.105:8554/test", clean)

host, port, user, pw, clean = rp.parse_url(
    "rtsp://admin:p%40ss@192.168.10.12:554/Streaming/Channels/101")
check("带认证地址解出用户名密码", (user, pw) == ("admin", "p@ss"), f"{user} {pw}")
# 认证信息必须从请求行里去掉，留着的话有些相机会直接 400
check("请求行里不含密码", "p@ss" not in clean and "admin" not in clean, clean)

host, port, _, _, _ = rp.parse_url("rtsp://192.168.1.105/live")
check("省略端口时默认 554", port == 554, str(port))


# --- 端到端：起一个假 RTSP 服务端 --------------------------------------------
print("\n== 端到端探测 ==")


class FakeRtsp(threading.Thread):
    """只回 DESCRIBE 的最小 RTSP 服务端，用来验证请求真的发得出去、SDP 收得回来。"""

    def __init__(self, require_auth=False):
        super().__init__(daemon=True)
        self.require_auth = require_auth
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]

    def run(self):
        try:
            conn, _ = self.sock.accept()
        except OSError:
            return
        try:
            authed = not self.require_auth
            while True:
                req = conn.recv(4096).decode("utf-8", "replace")
                if not req:
                    break
                cseq = "1"
                for line in req.split("\r\n"):
                    if line.lower().startswith("cseq:"):
                        cseq = line.split(":", 1)[1].strip()
                if self.require_auth and "Authorization:" not in req:
                    conn.sendall((
                        "RTSP/1.0 401 Unauthorized\r\n"
                        f"CSeq: {cseq}\r\n"
                        'WWW-Authenticate: Digest realm="x30", '
                        'nonce="abc123"\r\n\r\n').encode())
                    continue
                authed = True
                if authed:
                    conn.sendall(("RTSP/1.0 200 OK\r\n"
                                  f"CSeq: {cseq}\r\n"
                                  "Content-Type: application/sdp\r\n"
                                  f"Content-Length: {len(SDP)}\r\n\r\n"
                                  + SDP).encode())
                    break
        except OSError:
            pass
        finally:
            conn.close()


srv = FakeRtsp()
srv.start()
time.sleep(0.1)
code, sdp = rp.describe("127.0.0.1", srv.port, None, None,
                        f"rtsp://127.0.0.1:{srv.port}/test")
check("DESCRIBE 拿到 200", code == 200, str(code))
check("收到 SDP", "m=video" in sdp, sdp[:80])

srv2 = FakeRtsp(require_auth=True)
srv2.start()
time.sleep(0.1)
code, sdp = rp.describe("127.0.0.1", srv2.port, "admin", "pw",
                        f"rtsp://127.0.0.1:{srv2.port}/test")
check("401 后用 Digest 重试成功", code == 200, str(code))

# 连不上时要给出可操作的提示，而不是抛栈
print("\n== 连不上时的表现 ==")
r = subprocess.run(
    [sys.executable, __file__.replace("rtsp_probe_test.py", "rtsp_probe.py"),
     "rtsp://127.0.0.1:1/nothing", "--timeout", "2"],
    capture_output=True, text=True, timeout=30)
check("非零退出码", r.returncode != 0, str(r.returncode))
check("给出可操作提示", "连不上" in r.stdout, r.stdout[-200:])
check("没有抛栈", "Traceback" not in r.stderr, r.stderr[-200:])

print(f"\n通过 {PASS}，失败 {FAIL}")
sys.exit(0 if FAIL == 0 else 1)
