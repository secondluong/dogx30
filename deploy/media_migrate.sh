#!/usr/bin/env bash
# mediamtx.yml 的就地迁移。被 install.sh 引用，也被 tools/install_dryrun.sh 验证。
#
# 为什么要迁移而不是覆盖：现场那份 mediamtx.yml 里有相机地址和 RTSP 密码，
# 是一台一改的东西，install.sh 刻意不覆盖它。可 WebRTC 的监听方式又必须改 ——
# 老写法把它绑在单一地址上，而 MESH 与 2.4G 落在两个互不可达的网段，
# 绑死一个，另一条链路必然停在「等待拉流」。所以只能只改 webrtc* 那几行。
#
# 来龙去脉见 docs/media-architecture.md 第六节。

# migrate_mediamtx_webrtc <mediamtx.yml 路径>
#
# 可反复执行：已是新写法就直接返回。改动前先备份，迁移结果缺键则整份退回 ——
# 宁可没改成，也不能留下一个起不来的 mediamtx：没视频还能控狗，
# 配置坏了连服务都拉不起来。
migrate_mediamtx_webrtc() {
  local f="$1"
  [[ -f "$f" ]] || return 0
  if grep -qE '^webrtcIPsFromInterfacesList:' "$f"; then
    return 0
  fi

  local bak
  bak="$f.bak-$(date +%Y%m%d%H%M%S)"
  cp -p "$f" "$bak"

  local tmp="$f.new"
  awk '
    # 取 "1.2.3.4:8889" 或 ":8889" 里的端口
    function portof(v,  n, a) { n = split(v, a, ":"); return a[n] }
    /^webrtcAddress:/ {
      print "webrtcAddress: :" portof($2) \
            "                # 监听全部网卡：MESH 与 2.4G 在两个网段上"
      next
    }
    /^webrtcLocalUDPAddress:/ {
      p = portof($2)
      print "webrtcLocalUDPAddress: :" p
      print "webrtcLocalTCPAddress: :" p "        # UDP 不通时的 ICE 兜底"
      next
    }
    /^webrtcIPsFromInterfaces:/ {
      print "webrtcIPsFromInterfaces: yes"
      print "webrtcIPsFromInterfacesList: [eth0, eth1, wlan0]  # 刻意不列 wwan0（4G）"
      next
    }
    /^webrtcAdditionalHosts:/ { print "webrtcAdditionalHosts: []"; next }
    # 半迁移过的文件里可能已有这两行，去重
    /^webrtcLocalTCPAddress:/ { next }
    /^webrtcIPsFromInterfacesList:/ { next }
    { print }
  ' "$bak" > "$tmp"

  local ok=1 key
  for key in webrtcAddress webrtcLocalUDPAddress webrtcLocalTCPAddress \
             webrtcIPsFromInterfacesList; do
    grep -qE "^$key:" "$tmp" || ok=0
  done
  if [[ "$ok" == "1" ]]; then
    mv "$tmp" "$f"
    echo "已迁移 $(basename "$f") 的 WebRTC 监听方式（备份 $(basename "$bak")）"
  else
    rm -f "$tmp"
    echo "警告：$(basename "$f") 的 WebRTC 段不是预期格式，未改动。"
    echo "      2.4G 链路会拉不到流，请照 deploy/mediamtx.yml 手工改 webrtc 那几行。"
    return 1
  fi
}
