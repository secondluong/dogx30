# dogx30

绝影 X30 四足机器人的 RK3588 载荷控制系统。

RK3588 开发板挂载在机器狗背上，作为载荷计算机接入机身内网，对下与机器人的运动主机、
感知主机通信，对上与安卓平板通信，实现姿态/步态遥控、激光建图定位与视频回传。

## 当前进度

**遥控链路已打通**：遥控端操作 → RK3588 网关 → 机器狗运动主机，全程可用。
包含 X30 UDP 协议层、遥测解析、50 Hz 轴指令、安全看门狗、控制权仲裁、
上下楼时序编排、WebSocket 服务、Web 控制台与安卓 App，
以及一个不需要实机就能联调的仿真器。

**视频与对讲的底座已就位**：媒体源编排、解码能力协商、带宽仲裁、WHEP 拉流、
按住说话都已实现并有测试覆盖，选型依据见
[docs/media-architecture.md](docs/media-architecture.md)。
机身相机确认走 RTSP（`rtsp://192.168.1.105:8554/test`），已配好。
接实机还差球机的音频回传方式，那个只能拿到型号后确认。

**物理手柄已接入**：死区、曲线整形、按键上升沿、步态循环、掉线归零都已实现，
映射由 `gamepad.html` 向导生成后一键写入控制台。标准布局的手柄免配置直接可用。
物理摇杆一离开死区就自动接管，松开交还触摸摇杆。
另有可选的**死人开关**（按住才能推动，松手即停），默认关闭。
仍待现场验证的只剩一件事：**那台地面站的摇杆到底能不能被 Android 读到**
（有些是直接走 S.BUS 到电台的）。

**激光点云已打通**：网关直接说 ROS 的线上协议订阅 `/lidar_points`，
体素降采样加坐标量化后以二进制帧下行，遥控端 WebGL 渲染。
实测单帧 88 KB、2 Hz，约 1.44 Mbps，落在 MESH 预算内。
板子上**不需要装 ROS**，理由见
[docs/pointcloud-architecture.md](docs/pointcloud-architecture.md)。
整条链路有仿真器和端到端测试覆盖，但**感知主机的 ROS 可达性仍待现场验证**，
所以点云默认关闭，验通了再用 `--cloud` 打开。

网关是**零第三方依赖**的单个 C++ 二进制，HTTP、WebSocket、XML-RPC、TCPROS
都是自实现的。
嵌入式部署时少一个依赖就少一处交叉编译的坑，也避开了在 ARM 上装 Node 那套麻烦。
媒体路由是**独立进程**（MediaMTX），故意不合并进网关——网关承载急停和轴指令，
不能被视频解析的崩溃带走。

## 目录

| 路径 | 说明 |
| --- | --- |
| `rk3588/` | 载荷计算机侧 C++17 代码，编译出单个 `x30_gateway` |
| `web/` | Web 控制台，由网关直接托管 |
| `web/gamepad.html` | 手柄诊断页，验证物理摇杆可读性并生成按键映射 |
| `android-app/` | 平板遥控 App |
| `web/media.js` | 视频与对讲：解码能力探测、WHEP 拉流、按住说话 |
| `web/cloud.js` | 点云渲染：WebGL 直接吃 int16，不引 three.js |
| `web/gamepad.js` | 物理手柄输入：映射、死区、曲线、上升沿、掉线归零 |
| `deploy/` | systemd 服务与一键安装脚本 |
| `deploy/make_installer.sh` | **打单文件自解压安装包**，拷到板子上一条命令装完 |
| `deploy/install_gui.sh` | 向导式安装：检查环境、调优、编译、问参数、装服务、体检 |
| `deploy/checkup.sh` | **装完跑一遍**：整条链路体检，每项要么过、要么说下一步做什么 |
| `deploy/package.sh` | 打离线部署包，用于把源码送上没有网的板子 |
| `deploy/render_unit.sh` | 生成 systemd 单元，安装与测试共用一份，防止两边漂 |
| `deploy/media.json` | 媒体源清单样例（网关读） |
| `deploy/mediamtx.yml` | MediaMTX 配置样例（媒体服务读） |
| `tools/all_tests.sh` | 跑全部本地测试，改完代码和上板子前都该过一遍 |
| `tools/x30_sim.py` | 机器狗运动主机仿真器，用于无实机开发 |
| `tools/ros_sim.py` | 假 ROS master + 点云发布者，说的是真 ROS 的线上协议 |
| `tools/smoke_test.sh` | 运动控制层冒烟测试 |
| `tools/serve_test.sh` | 遥控服务端到端测试 |
| `tools/ws_probe.py` | 遥控协议探针（手写 WebSocket 客户端） |
| `tools/cloud_probe.py` | 点云探针：解回坐标验格式、精度、抽帧、退订 |
| `tools/rtsp_probe.py` | 探 RTSP 流的编码/分辨率/音轨，用来填 `media.json`，不依赖 ffmpeg |
| `tools/gamepad_test.js` | 手柄输入层测试：死区、符号约定、上升沿、掉线归零 |
| `tools/shutdown_test.sh` | 关停耗时测试，对端不可达时也要能干净退出 |
| `tools/install_dryrun.sh` | 空跑安装脚本的参数替换 |
| `tools/checkup_test.sh` | 验体检脚本能把安装脚本写下的参数原样读回来 |
| `tools/installer_test.sh` | 验自解压包解得出完整源码，且不该交互时不会挂住 |
| `tools/state_once.py` | 连一次网关取遥测，供体检脚本判断"是否真收得到数据" |
| `tools/fix_eol.py` | 把 CRLF 换成 LF，Windows 上编辑后跑一下 |
| `tools/bind_check.sh` | 验证 `--bind` 确实把服务限制在指定网卡上 |
| `tools/check_scripts.sh` | shell 脚本静态检查 |
| `docs/system-architecture.md` | **系统架构、网络拓扑、当前进度** |
| `docs/media-architecture.md` | 视频与对讲：选型依据、带宽策略、待确认项 |
| `docs/pointcloud-architecture.md` | 点云：为什么不装 ROS、带宽怎么砍、踩过的坑 |
| `docs/rk3588-setup.md` | **RK3588 首次部署，从空板子到能遥控** |
| `docs/app-protocol.md` | RK3588 ↔ 平板的 WebSocket 协议 |
| `docs/x30-protocol.md` | 从官方 API 文档提炼的接口速查 |
| `docs/hardware-integration.md` | 硬件接入步骤与现场验证清单 |
| `docs/reference-mysystem.md` | 既有 MyView 系统里可借鉴的部分 |

## 快速开始（无实机）

需要 CMake ≥ 3.16、支持 C++17 的编译器、Python ≥ 3.8。

Windows 上没有 C++ 工具链的话用 WSL，它的 Ubuntu + GCC 与 RK3588 的目标环境
（同为 LP64 Linux）最接近，遥测结构体的对齐行为也一致：

```bash
wsl -d Ubuntu-22.04
cd /mnt/d/CODES/cursor/dogx30
```

编译：

```bash
cmake -S rk3588 -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

两个测试。运动控制层：

```bash
bash tools/smoke_test.sh build/x30_gateway
```

它会拉起仿真器，走一遍起立 → 力控 → 踏步 → 行走 → 看门狗超时 → 急停，
并断言每一步的遥测解析结果。

遥控服务（含 HTTP、WebSocket、控制权仲裁、急停免鉴权）：

```bash
bash tools/serve_test.sh
```

**用浏览器实际操作一遍**，这是最直观的验证方式：

```bash
python3 tools/x30_sim.py &
./build/x30_gateway --robot-ip 127.0.0.1 --serve --web web
```

然后打开 <http://localhost:8080/>。先点「申请控制权」，再依次点
「坐 / 站」→「力控站立」→「起步 / 停步」，之后左摇杆就能让它走起来，
遥测盘上的速度和里程计会实时跟着变。

想用命令行调试就开两个终端。先起仿真器：

```bash
python3 tools/x30_sim.py
```

再起网关，指向本机：

```bash
./build/x30_gateway --robot-ip 127.0.0.1 --interactive
```

依次敲 `stand`、`torque`、`step`，然后 `v 0.5 0 0` 让它前进，`?` 列出全部命令。
仿真器那侧会打印状态迁移，能直接看出指令有没有被正确解析。

接实机时换掉 IP：

```bash
./build/x30_gateway --robot-ip 192.168.1.103 --local-port 43897 --interactive
```

实机还需要先在运动主机上登记本机 IP，否则一条遥测都收不到，
见 [docs/hardware-integration.md](docs/hardware-integration.md)。

## 部署到 RK3588

板子是第一次用的话，走完整流程：**[docs/rk3588-setup.md](docs/rk3588-setup.md)**。
它从烧录系统讲到平板连上控制台，并且刻意把所有软件工作安排在桌面上完成、
用仿真器验证过之后再上狗——狗身上没有显示器也没有互联网，趴在地上调板子
既慢又危险。

工程还没有 git 远端，板子首次上电多半也没网，所以走离线包。
**在开发机上**生成单文件安装包：

```bash
bash deploy/make_installer.sh   # → dist/x30-installer-<日期>.run，约 230 KB
```

用 `scp` 或 U 盘（**FAT32**，很多板子镜像不带 exFAT 驱动）送过去，然后：

```bash
chmod +x x30-installer-*.run
sudo ./x30-installer-*.run      # 桌面上双击也行，会自己开终端
```

它把环境检查、系统调优、编译、参数确认、装服务、体检串成一条，做过的步骤
自动跳过，可反复跑。源码留一份在 `~/dogx30`，桌面上生成控制台/体检/日志
三个快捷方式。加 `--yes` 全用默认值不提问，加 `--extract <目录>` 只解压不装。

包里只有源码，二进制在板子上现编（整个工程只依赖编译器，十几秒）。
打包时会把换行统一成 LF —— 不只是脚本，`.service` 和 `.json` 也一样，
systemd 单元带 CR 会让反斜杠续行断掉，症状极隐蔽。

想分步手工装（第一次建议这样，能看清每步在干什么）：

```bash
bash deploy/package.sh          # → dist/dogx30-<日期>.tar.gz
# 送上板子解压后
sudo bash deploy/bootstrap.sh --static-ip 192.168.1.120/24 --iface eth0
sudo bash deploy/install.sh --robot-ip 192.168.1.103 --perception-ip 192.168.1.105
bash deploy/checkup.sh
```

`--perception-ip` 指向地形图模块，**上下楼步态必需**，缺了它步态指令会被
静默忽略。两个脚本都幂等，可反复执行以升级。

**本机若接有 4G 或其他广域接口**，务必加 `--bind <遥控链路地址>`，
把服务限制在遥控网卡上——协议还没有身份认证，见下。

```bash
journalctl -u x30-gateway -f      # 看日志
systemctl restart x30-gateway     # 重启
```

平板接入有两种方式，任选其一：

- 装 `android-app/` 里的 App，填 RK3588 的 IP 即可。构建见该目录的 README。
- 直接用平板浏览器打开 `http://<RK3588_IP>:8080/`。功能完全一样，
  但没有屏幕常亮、锁横屏、返回键保护，只适合临时调试。

## 安全设计

遥控系统里最要紧的是"失联时会发生什么"。三层兜底，缺一不可：

| 故障 | 兜底机制 | 生效时间 |
| --- | --- | --- |
| 平板停发指令（App 卡死、切后台） | 网关看门狗清零轴指令 | 300 ms |
| 平板断连或掉电 | 释放控制权并立即清零 | 立即 / 租约 3 s |
| RK3588 宕机或网络中断 | 机器狗按 `overtime.toml` 自行降级停下 | 由机器人配置决定 |

另外，同一时刻只有一个客户端能控制机器狗（两台平板同时推摇杆是真实的安全隐患），
但**急停不受控制权限制**，任何客户端任何时候都能触发。

上面三条防的是链路故障，不是恶意接入：**协议目前没有身份认证**，
凡能连上 8080 的客户端都能申请控制权。这在隔离的遥控局域网里可以接受，
一旦本机接入 4G 或公网就不成立。补上认证之前靠部署收敛暴露面——
用 `--bind` 限定监听网卡、用防火墙只放行遥控链路，
做法见 [docs/rk3588-setup.md 第 18 节](docs/rk3588-setup.md)。

## 系统架构

完整拓扑、MESH 与 4G 的分工、交换机选型见
[docs/system-architecture.md](docs/system-architecture.md)。要点：

```
机器狗机身                    RK3588 载荷                 安卓手持地面站
┌──────────────────┐        ┌──────────────────┐        ┌──────────┐
│ 运动主机 .103    │◄─UDP──►│ MotionClient     │        │  摇杆    │
│   43893          │        │ TerrainClient    │        │  视频    │
│ 感知主机 .105    │◄─UDP──►│ GaitCoordinator  │◄─WS───►│  布控球  │
│   43899 地形图   │        │ RobotService     │  MESH  │  气体    │
│   ROS1 点云/IMU  │◄─ROS1─►│ 视频转码出口     │        │          │
│ 4x Livox Mid-360 │        └──────────────────┘        └──────────┘
└──────────────────┘          │            │ 4G（运维/云端）
                              └─ 交换机 ─── 布控球 / 气体监测
```

**运动控制与地形图在两台不同主机上，楼梯步态必须两条通道配合**，
只发步态指令会被静默忽略。这是本项目最容易踩的坑，已由 `GaitCoordinator`
收敛成一个动作并用步态遥测确认生效。

分工原则：机器人本体的运动学与地形感知一律不动。步态规划、落足点计算、避障
继续由机器人自己负责，RK3588 只做网关、SLAM、编码与协议转换。

## 路线图

- [x] X30 UDP 协议层与遥测解析
- [x] 运动网关与安全看门狗
- [x] 无实机仿真器（含地形图主机）
- [x] RK3588 ↔ 遥控端 WebSocket 协议与控制权仲裁
- [x] 零依赖 HTTP + WebSocket 服务
- [x] Web 控制台：虚拟摇杆、遥测盘、急停
- [x] 安卓 App 与 systemd 部署
- [x] 地形图通道与上下楼时序编排（**待实机验证**）
- [x] 离线部署包（`deploy/package.sh`）
- [x] 媒体编排：源清单、能力协商、带宽仲裁、WHEP 拉流、按住说话
- [x] 点云：零依赖 ROS 桥接、降采样量化下行、WebGL 渲染（**待现场验证 ROS 可达性**）

按现场确认的优先级，剩下的排成六项：

- [ ] **① 部署到 RK3588** —— 代码与文档已就绪，等现场执行
- [ ] **② 手柄控制行走** —— 输入层已完成并有 91 条测试；等 `gamepad.html` 实测结果
- [x] **③ 手柄上看点云** —— 已完成当前帧；累积地图还要 LIO 配准
- [ ] **④ 狗自带相机视频** —— 确认走 RTSP 并已配好，待接实机验证
- [ ] **⑤ 双光布控球 + 云台 + 对讲** —— 视频走 RTSP 已就绪；
      云台与对讲走 GB28181，协议已定、SIP 层未写
- [ ] **⑥ 气体监测接入与告警**
- [ ] 点云进阶：LIO 建图定位，让遥控端看到累积地图而不只是当前帧
- [ ] 转码兜底路径（遥控端不支持 H.265 且相机只出 H.265 时）
- [ ] 4G 云端上传（明确排在最后）

## 已知风险

**机身相机不在官方文档里。** 官方 68 页 API 文档中 camera / image / video / rtsp
出现次数为零，但现场确认感知主机上确实开着 `rtsp://192.168.1.105:8554/test`。
既然是文档外的接口，就不能指望它稳定：换固件后地址或路径可能变，
也没有兼容性承诺。所以只把它当加分项，双光布控球仍是主力视频源。
取不到流时先用 `tools/rtsp_probe.py` 重探一遍。

**智能控制器到底有没有，说法变了。** 早期资料称本机不含智能控制器，据此推出
「没有 `.106`、官方 `jy_cog` 建图定位不可用、SLAM 必须自建」。但厂家 2026-08
给的拓扑图上**明确画着智能控制器**（`.106` / `.2.106`）。一条 ping 就能定论，
`deploy/checkup.sh` 里带了。若它真在，自建 SLAM 这块工作量可能整个省掉。
供电与它无关，机身仍只有 72V 口，需自备 72V→12V DC-DC 模块。

**尾部调试网口经过一个路由器。** 板子从这里接入，而拓扑图显示它到交换机之间
隔着路由器。若那是 NAT 而非桥接，板子就不在 `192.168.1.0/24` 里，运动主机的
单播遥测永远回不来 —— 症状和「`network.toml` 没登记」一模一样。
上狗第一件事是插网线用 DHCP 看拿到哪个网段，别急着配静态地址。

**SLAM 算力余量偏紧。** 四雷达合并云每帧约 8 万点，原版 FAST-LIO2 在 ARM 上单雷达
已需约 40 ms/帧，必须靠降采样压到 2 万点量级。
