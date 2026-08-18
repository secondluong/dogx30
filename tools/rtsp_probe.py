#!/usr/bin/env python3
"""探测一路 RTSP 流的编码、分辨率与音轨，用来填 deploy/media.json。

为什么不用 ffprobe：板子上不一定装了 ffmpeg，而这一步恰恰是**装机当天**要做的。
RTSP 的 DESCRIBE 本身就会回一段 SDP，编码和音轨信息都在里面，
自己发一个请求比先装 200 MB 的 ffmpeg 快得多。分辨率从 H.264 的 SPS 里解。

media.json 里 codec 和 kbps 填错不会黑屏，但会让带宽仲裁算错账 ——
表现为"明明只看一路却卡"，很难联想到是配置写错了。所以值得实测一次。

用法:
    python3 tools/rtsp_probe.py rtsp://192.168.1.105:8554/test
    python3 tools/rtsp_probe.py rtsp://admin:pw@192.168.10.12:554/Streaming/Channels/101
"""

import argparse
import base64
import hashlib
import re
import socket
import sys
import urllib.parse


def parse_url(url):
    u = urllib.parse.urlparse(url)
    if u.scheme != "rtsp":
        raise ValueError(f"不是 rtsp:// 地址: {url}")
    host = u.hostname
    port = u.port or 554
    user = urllib.parse.unquote(u.username) if u.username else None
    pw = urllib.parse.unquote(u.password) if u.password else None
    # 认证信息不能留在请求行里，要抽出来单独走 Authorization 头
    netloc = host if u.port is None else f"{host}:{u.port}"
    clean = urllib.parse.urlunparse(
        ("rtsp", netloc, u.path or "/", u.params, u.query, ""))
    return host, port, user, pw, clean


def digest_header(user, pw, method, uri, chal):
    def field(name):
        m = re.search(name + r'="([^"]*)"', chal)
        return m.group(1) if m else ""

    realm, nonce = field("realm"), field("nonce")
    ha1 = hashlib.md5(f"{user}:{realm}:{pw}".encode()).hexdigest()
    ha2 = hashlib.md5(f"{method}:{uri}".encode()).hexdigest()
    resp = hashlib.md5(f"{ha1}:{nonce}:{ha2}".encode()).hexdigest()
    return (f'Digest username="{user}", realm="{realm}", nonce="{nonce}", '
            f'uri="{uri}", response="{resp}"')


def describe(host, port, user, pw, url, timeout=5.0):
    """发 DESCRIBE，返回 (状态码, SDP 文本)。自动处理 401。"""
    sock = socket.create_connection((host, port), timeout=timeout)
    sock.settimeout(timeout)
    seq = [0]

    def request(auth=None):
        seq[0] += 1
        lines = [f"DESCRIBE {url} RTSP/1.0",
                 f"CSeq: {seq[0]}",
                 "Accept: application/sdp",
                 "User-Agent: x30-rtsp-probe"]
        if auth:
            lines.append(f"Authorization: {auth}")
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())

        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = sock.recv(4096)
            if not chunk:
                raise RuntimeError("连接被对端关闭")
            buf += chunk
        head, _, rest = buf.partition(b"\r\n\r\n")
        head = head.decode("utf-8", "replace")

        m = re.search(r"Content-Length:\s*(\d+)", head, re.I)
        want = int(m.group(1)) if m else 0
        while len(rest) < want:
            chunk = sock.recv(4096)
            if not chunk:
                break
            rest += chunk
        code = int(head.split()[1]) if len(head.split()) > 1 else 0
        return code, head, rest.decode("utf-8", "replace")

    code, head, body = request()
    if code == 401 and user:
        chal = ""
        for line in head.split("\r\n"):
            if line.lower().startswith("www-authenticate:"):
                chal = line.split(":", 1)[1].strip()
                break
        if chal.lower().startswith("digest"):
            auth = digest_header(user, pw or "", "DESCRIBE", url, chal)
        else:
            token = base64.b64encode(f"{user}:{pw or ''}".encode()).decode()
            auth = f"Basic {token}"
        code, head, body = request(auth)

    sock.close()
    return code, body


class BitReader:
    """H.264 SPS 用的比特流读取器，带 RBSP 去竞争字节处理。"""

    def __init__(self, data):
        # 0x000003 里的 0x03 是防竞争字节，解析前必须去掉，
        # 否则遇到含该模式的 SPS 会算出离谱的分辨率
        out = bytearray()
        i = 0
        while i < len(data):
            if i + 2 < len(data) and data[i] == 0 and data[i + 1] == 0 \
                    and data[i + 2] == 3:
                out += data[i:i + 2]
                i += 3
            else:
                out.append(data[i])
                i += 1
        self.d = bytes(out)
        self.pos = 0

    def bit(self):
        if self.pos >= len(self.d) * 8:
            raise ValueError("SPS 数据不足")
        b = (self.d[self.pos >> 3] >> (7 - (self.pos & 7))) & 1
        self.pos += 1
        return b

    def bits(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | self.bit()
        return v

    def ue(self):
        z = 0
        while self.bit() == 0:
            z += 1
            if z > 32:
                raise ValueError("指数哥伦布码异常")
        return (1 << z) - 1 + (self.bits(z) if z else 0)

    def se(self):
        k = self.ue()
        return (k + 1) // 2 if k % 2 else -(k // 2)


def h264_resolution(sps_bytes):
    """从 H.264 SPS 解出宽高。失败返回 None —— 分辨率是锦上添花，不该让整个探测失败。"""
    try:
        r = BitReader(sps_bytes[1:])  # 跳过 NAL 头
        profile = r.bits(8)
        r.bits(8)                     # constraint flags + reserved
        r.bits(8)                     # level_idc
        r.ue()                        # seq_parameter_set_id

        chroma = 1
        if profile in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139,
                       134, 135):
            chroma = r.ue()
            if chroma == 3:
                r.bit()               # separate_colour_plane_flag
            r.ue()                    # bit_depth_luma_minus8
            r.ue()                    # bit_depth_chroma_minus8
            r.bit()                   # qpprime_y_zero_transform_bypass
            if r.bit():               # seq_scaling_matrix_present
                for i in range(8 if chroma != 3 else 12):
                    if r.bit():
                        size = 16 if i < 6 else 64
                        last, nxt = 8, 8
                        for _ in range(size):
                            if nxt:
                                nxt = (last + r.se() + 256) % 256
                            last = nxt or last

        r.ue()                        # log2_max_frame_num_minus4
        order = r.ue()
        if order == 0:
            r.ue()
        elif order == 1:
            r.bit()
            r.se()
            r.se()
            for _ in range(r.ue()):
                r.se()
        r.ue()                        # max_num_ref_frames
        r.bit()                       # gaps_in_frame_num_value_allowed

        w_mbs = r.ue() + 1
        h_maps = r.ue() + 1
        frame_mbs_only = r.bit()
        if not frame_mbs_only:
            r.bit()                   # mb_adaptive_frame_field_flag
        r.bit()                       # direct_8x8_inference_flag

        width = w_mbs * 16
        height = (2 - frame_mbs_only) * h_maps * 16

        if r.bit():                   # frame_cropping_flag
            l, rr, t, b = r.ue(), r.ue(), r.ue(), r.ue()
            sub_w = 1 if chroma == 3 else 2
            sub_h = 1 if chroma in (2, 3) else 2
            if not frame_mbs_only:
                sub_h *= 2
            width -= (l + rr) * sub_w
            height -= (t + b) * sub_h
        return width, height
    except (ValueError, IndexError):
        return None


def parse_sdp(sdp):
    """拆出各条 media 轨及其编码。"""
    tracks = []
    cur = None
    for raw in sdp.splitlines():
        line = raw.strip()
        if line.startswith("m="):
            parts = line[2:].split()
            cur = {"kind": parts[0], "codec": None, "fmtp": None}
            tracks.append(cur)
        elif cur is None:
            continue
        elif line.startswith("a=rtpmap:"):
            m = re.match(r"a=rtpmap:\d+\s+([A-Za-z0-9_\-]+)", line)
            if m and cur["codec"] is None:
                cur["codec"] = m.group(1).upper()
        elif line.startswith("a=fmtp:"):
            cur["fmtp"] = line
    return tracks


def main():
    p = argparse.ArgumentParser(description="探测 RTSP 流，用于填 media.json")
    p.add_argument("url")
    p.add_argument("--timeout", type=float, default=5.0)
    args = p.parse_args()

    try:
        host, port, user, pw, clean = parse_url(args.url)
    except ValueError as e:
        print(f"地址有误: {e}")
        return 2

    print(f"探测 {clean}")
    try:
        code, sdp = describe(host, port, user, pw, clean, args.timeout)
    except (OSError, RuntimeError) as e:
        print(f"\n连不上: {e}")
        print("  先确认本机能 ping 通该地址，且 RTSP 端口没被防火墙挡。")
        return 1

    if code == 401:
        print("\n需要认证，但没给用户名密码，或密码不对。")
        print("  用 rtsp://用户名:密码@地址/路径 的形式重试。")
        return 1
    if code == 404:
        print("\n404：地址通了但这个路径没有流。")
        print("  路径写错，或相机换了固件改了路径。")
        return 1
    if code != 200:
        print(f"\nRTSP 返回 {code}")
        return 1

    tracks = parse_sdp(sdp)
    video = [t for t in tracks if t["kind"] == "video"]
    audio = [t for t in tracks if t["kind"] == "audio"]

    print("\n---- 探测结果 ----")
    if not video:
        print("没有视频轨（SDP 里没有 m=video）")
        return 1

    v = video[0]
    codec = (v["codec"] or "未知")
    print(f"视频编码 : {codec}")

    size = None
    if codec == "H264" and v["fmtp"]:
        m = re.search(r"sprop-parameter-sets=([^;,\s]+)", v["fmtp"])
        if m:
            try:
                size = h264_resolution(base64.b64decode(m.group(1) + "=="))
            except Exception:
                size = None
    if size:
        print(f"分辨率   : {size[0]}x{size[1]}")
    elif codec == "H265":
        print("分辨率   : 未解析（H.265 的 SPS 结构复杂，这里不解）")
    else:
        print("分辨率   : 未能解析")

    if audio:
        print(f"音频     : 有，{audio[0]['codec'] or '未知编码'}")
    else:
        print("音频     : 无")

    print("\n---- media.json 该怎么填 ----")
    json_codec = {"H264": "h264", "H265": "h265"}.get(codec)
    if json_codec is None:
        print(f'codec 填什么不确定：SDP 报的是 "{codec}"，')
        print("WebRTC 只支持 H.264/H.265，这路流可能需要转码。")
    else:
        label = f"{size[1]}p" if size else "按实际填"
        print(f'"codec": "{json_codec}"')
        print(f'"label": "{label}"       （帧率要另外看，SDP 里通常没有）')
        print(f'"audio": {"true" if audio else "false"}')
        print()
        print("kbps 要实测：让这一路跑起来，用 `iftop` 或看 MediaMTX 的")
        print("统计接口 `curl -s http://127.0.0.1:9997/v3/paths/list`。")
        print("填的是**给带宽仲裁算账用的**，宁可略高不要偏低。")

    if json_codec == "h265":
        print()
        print("注意：H.265 在 WebRTC 里没有软解兜底。遥控端 WebView < 136")
        print("或 SoC 没有 HEVC 硬解就完全看不了，届时必须转码。")

    return 0


if __name__ == "__main__":
    sys.exit(main())
