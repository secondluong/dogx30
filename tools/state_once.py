#!/usr/bin/env python3
"""连一次遥控服务，报告是否真的收到了机器狗的遥测。

    python3 tools/state_once.py 127.0.0.1 8080

输出三种之一，供 deploy/checkup.sh 判断：

    ALIVE <摘要>   网关连得上，且正在收到运动主机的数据
    STALE <摘要>   网关连得上，但收不到数据 —— 多半是 network.toml 没登记本机
    其它文本       连不上遥控服务本身

把这两种情况分开很重要。"能 ping 通"和"收得到遥测"是两回事，
后者失败时现场看到的是遥测盘全灰，很容易误判成网关坏了，
实际上要去改机器狗上的 network.toml。
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def main():
    if len(sys.argv) < 3:
        print("用法: state_once.py <host> <port>")
        return 2

    try:
        from ws_probe import WsClient
    except Exception as e:                                  # noqa: BLE001
        print("导入 ws_probe 失败: %s" % e)
        return 2

    host, port = sys.argv[1], int(sys.argv[2])
    try:
        c = WsClient(host, port)
    except Exception as e:                                  # noqa: BLE001
        print("连不上: %s" % e)
        return 2

    try:
        c.wait_for("hello", timeout=5)
        st = c.wait_for("state", timeout=5)
    except Exception as e:                                  # noqa: BLE001
        print("连上了但没收到状态: %s" % e)
        return 2
    finally:
        try:
            c.close()
        except Exception:                                   # noqa: BLE001
            pass

    summary = json.dumps({
        "状态": st.get("basic_state_text", ""),
        "步态": st.get("gait_text", ""),
        "电量": st.get("battery", {}).get("level"),
    }, ensure_ascii=False)

    print(("ALIVE " if st.get("alive") else "STALE ") + summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
