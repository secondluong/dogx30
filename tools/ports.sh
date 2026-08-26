#!/usr/bin/env bash
# 给测试脚本挑一个当下没人占的端口。
#
#   source tools/ports.sh
#   PORT=$(free_port tcp)
#   LOCAL_PORT=$(free_port udp)
#
# 为什么不写死端口：开发板上装好的那份服务一直跑着，占着 8080 和 43897；
# 两个人同时跑一轮测试还会再撞一次。撞上去的后果并不总是干脆的失败 ——
# TCP 那侧网关起不来，整轮场景全灭，但报出来的是一堆协议断言失败；
# UDP 那侧更阴，两个进程都绑得上，内核把仿真器的遥测轮流分给它们，
# 被测网关只收到一半，于是看门狗、急停这些断言随机红。查到最后都是环境。

# free_port [tcp|udp]  —— 让内核挑，避免自己维护一张"应该没人用"的端口表。
# 拿到号就把探测用的套接字关掉，理论上存在被别人抢走的窗口，
# 但取的是临时端口段里的号，实际撞不上。
free_port() {
  python3 -c '
import socket, sys
kind = socket.SOCK_DGRAM if sys.argv[1] == "udp" else socket.SOCK_STREAM
s = socket.socket(socket.AF_INET, kind)
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
' "${1:-tcp}"
}
