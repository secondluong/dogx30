# 参考：MyView（mysystem）

我方既有系统，位于 `D:\CODES\cursor\mysystem`，远端 `github.com/secondluong/mysystem`。
本文只记录 dogx30 可能借鉴的部分，不是那套系统的完整文档。

## 它是什么

音视频调度平台，五块组成：

| 模块 | 技术 | 角色 |
| --- | --- | --- |
| `server/` | Node 18 + Express + ws + better-sqlite3 | 后端，同时托管前端 |
| `client/` | React + Vite | 前端，构建到 `client/dist/` |
| `livekit/` | LiveKit SFU 二进制 | WebRTC 媒体转发 |
| `android-app/` | 原生 Java + `io.github.webrtc-sdk:android` | 安卓终端 |
| `lidar-bridge-ros/` | Docker + ROS + Point-LIO | 随行计算机上的激光桥接 |

LiveKit 不单独部署，由 Node 后端 spawn 成子进程，日志带 `[LiveKit-SFU]` 前缀合并输出。

## 运行与端口

Windows 双击 `start.bat`（生产）/ `start-dev.bat`（Vite 热更新）/ `stop.bat`。
命令行是 `npm run install:all`、`npm run build`、`npm start`。

访问走 **`https://<IP>:3443`**。浏览器 `getUserMedia` 在非 localhost 下必须 HTTPS，
后端会把页面请求 302 到 3443；3000 只留给 App 的 WebSocket 信令与 `/api`。

| 端口 | 用途 |
| --- | --- |
| 3000 | HTTP + WS + API |
| 3443 | HTTPS（浏览器摄像头必须） |
| 5060 | GB28181 SIP |
| 7880 / 7881 | LiveKit |
| 1883 / 9001 | MQTT |
| 30000–40099 | RTP 媒体 |

数据在 `~/.myview/myview.db`（Linux 为 `/var/lib/myview/.myview/myview.db`），
证书在 `server/certs/`。升级时这两处要保留。

## 三条打包线

- Windows 单文件自解压 EXE：`build-installer.bat`，约 300–380 MB，自带 Node/FFmpeg/LiveKit
- Linux x86_64：`package-linux.sh` → `install.sh` → systemd
- Linux ARM64：`npm run package:uos-arm64`

## dogx30 值得借鉴的三处

### 1. `lidar-bridge-ros` 的桥接模式

现有形态：树莓派 5 直连宇树 L2，Docker 内跑 `unitree_lidar_ros` + `point_lio_unilidar`，
`relay/relay.py` 把 ROS 话题编码成 ULDR 二进制 + JSON 的 pose/imu/status，
经 WebSocket 推到调度机 `/ws/lidar`，浏览器用 Potree 渲染。上游算法零修改。

搬到 X30 更简单：机器狗自己已经在跑雷达驱动，RK3588 不需要驱动层，
只要让 LIO 订阅 `/lidar_points` 与 `/imu/data`。relay 的编码、上行、
断线重连、双网卡路由拆分逻辑可以直接参考。

**其中最值钱的一条经验**（README 明确写了，X30 上完全同理）：
必须转发世界系**已配准**点云 `/pointlio/cloud_registered`，
不能转发雷达原始扫描 `/unilidar/cloud`。原始扫描以雷达为原点、随设备移动
始终缩在原点附近，而轨迹是世界系累加坐标，两者坐标系不同会出现
"轨迹飞出点云空间"。

其他可借鉴的工程细节：
- 上电顺序无关的启动编排（等网卡 → 等雷达 → 等服务端可达 → 再起算法，约 30–40 秒）
- IMU 静置校准等待（`IMU_SETTLE_S`，执行两次），校准完成前不要移动
- 雷达断线重连后自动重启 LIO 并重新校准（watchdog）
- 浏览器端可切 3s / 10s / 30s 点云累积，静止扫描时提高密度
- `/api/lidar/preflight` 免登录自检端点，现场排障很方便

### 2. `packaging/uos-arm64` 的 ARM64 安装脚本

`vendor/` 预置 `node-v18.20.4-linux-arm64.tar.gz` 与 `livekit-server-linux-arm64`
支持完全离线安装。装 Node 的策略值得抄：每装一个构建都跑 `node -v` 验证，
段错误就自动换下一个（官方 arm64 → glibc 2.17 兼容构建），
显然是被国产 CPU 坑过。RK3588 上装东西可以直接复用这套思路。

另有两条已知坑记录在案：
- LiveKit 在 IPv4/IPv6 双栈下 ICE 可能选到走不通的 IPv6 候选导致 DTLS 超时，
  内网部署直接关 IPv6
- 双网卡跨网段部署要开「多网卡桥接」让 LiveKit 枚举所有网卡地址作为 ICE 候选，
  且此时「媒体公网 IP」必须留空

### 3. `android-app` 的既有能力

已有登录、WebSocket 自动重连、来电振动弹窗、WebRTC 视频对讲、
通话控制（静音/关摄像头/切前后摄/挂断）。缺的是摇杆遥控与点云渲染。

一个已踩的坑：部分安卓机硬件渲染出不来画面，App 内置了软件渲染兜底
（长按通话页顶部文字切换，约 10 fps，持久化）。

## 与 dogx30 当前进度的关系

dogx30 的运动控制层是独立的，不依赖 MyView。
后续做 SLAM 上行、视频链路、平板端时再回来对照这份笔记。
