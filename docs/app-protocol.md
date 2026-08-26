# RK3588 ↔ 遥控端协议

WebSocket，端点 `ws://<RK3588_IP>:8080/ws`，全部消息是 UTF-8 的扁平 JSON 对象，
用 `t` 字段区分类型。选 JSON 而不是 protobuf 的理由是控制通道流量极小
（20 Hz × 约 60 字节 ≈ 10 kbps），可读性带来的排障效率远比这点带宽值钱。
后面点云走独立的二进制通道，不在此协议内。

## 控制权仲裁

允许多个客户端同时连接（平板遥控 + 笔记本观察），但**同一时刻只有一个能控制**。
两台平板同时推摇杆是真实的安全隐患，所以控制权必须独占。

规则：

- 新连接默认是**观察者**，能收全部遥测，发控制指令会被拒。
- 发 `claim` 申请控制权。当前无人持有时立即授予。
- 持有者超过 `control_lease_ms`（默认 2000 ms）没发任何指令，租约自动过期，
  其他客户端才能接管。这样遥控端崩掉不会把控制权永久锁死。
- 持有者断开连接时立即释放，并同时把轴指令清零。
- **急停是唯一的例外**：任何客户端、任何时候都能发 `estop`，不需要控制权。
  安全动作绝不能因为权限判断而延迟。

## 客户端 → 服务端

### 控制权

```json
{"t":"claim"}                    // 申请控制权
{"t":"claim","standing":true}    // 顺带告知狗现在站着（见下）
{"t":"yield"}                    // 主动交还
```

`standing` 是**可选**的姿态交接。为什么需要它：2.4G 直连时起立/趴下由遥控器
直接打给运动主机，不经过网关；而运动主机在 RL 起立后遥测仍报 `basic_state=0`
（坐下），网关从遥测里也认不出来。于是遥控端从 2.4G 切回 MESH 后，网关记的还是
「坐着」—— 界面左下角显示「起立」，而且轴指令会被 `AxisCommandsApply` 吞掉，
狗明明站着却推不动。带上这个键，网关就把那份记忆对上，**不会向狗发任何指令**。

只在自己确实知道姿态时才带：遥控端发过起立/趴下，或者收到了狗的姿态遥测。
不知道就不要带 —— 猜错的表现是按「趴下」时狗反而站起来。不带这个键的 `claim`
不会改动网关的记忆（网页控制台就属于这种）。

采纳成功的标志是随后 `state` 里的 `rl_standing` 跟着变了，遥控端据此确认交接完成。
**旧网关会静默忽略这个键**，`rl_standing` 不会变；这时遥控端按自己知道的姿态显示
按钮（否则界面就在说谎），但轴仍会被网关吞掉，所以要提示操作员更新网关。

### 离散指令

```json
{"t":"cmd","name":"stand"}                  // 坐 <-> 站 切换
{"t":"cmd","name":"unload"}                 // 卸力：急停后解除关节自锁
{"t":"cmd","name":"torque"}                 // 进入力控站立
{"t":"cmd","name":"step"}                   // 起步 <-> 停步 切换
{"t":"cmd","name":"height","value":"normal"} // normal|crawl
{"t":"cmd","name":"mode","value":"manual"}   // manual|auto
{"t":"cmd","name":"estop"}                   // 软急停，无需控制权
{"t":"cmd","name":"savedata"}
```

注意机器人自身的控制逻辑优先：趴着的时候发 `step` 会被运动主机直接忽略。
所以遥控端的按钮状态应当跟着 `state` 推送走，而不是自己假设发了就生效。

### 步态切换（异步）

```json
{"t":"cmd","name":"gait","value":"walk","stair_style":"solid"}
```

`value` 取 `walk|slope|offroad|stair|stairmulti|stair45|lwalk|mountain|silent`。

`stair_style` 只对 `stair`（单帧楼梯）有意义，取 `solid|grating|noriser`，
分别对应实心踏面、格栅、无踢面，缺省 `solid`。选错的表现是步态切不过去。

**这条指令是异步的**，服务端处理完会回一条 `gait_result`。楼梯步态尤其慢：
要跨运动主机与感知主机按序设置四项，多帧模式还要先等机器狗停稳，
整个过程可能几秒。遥控端应当在等待期间禁用步态按钮。

服务端在楼梯步态上做的事，按顺序：

1. 切非手动模式（运动主机），否则地形图的修正结果不参与速度链路
2. 多帧模式下清零轴指令并等待机器狗静止
3. 障碍高度阈值设为 28cm（感知主机），留在 8cm 会把台阶当障碍
4. 设置地形图模式（感知主机），多帧要先"准备"再"启用"
5. 下发步态指令（运动主机）
6. 等待遥测确认步态真的变了

切到非楼梯步态时，障碍高度阈值会按文档推荐值自动回设（Walk/缓坡 8cm，
越野 28cm），不需要遥控端操心。

同一时刻只允许一次切换在途，重复请求会收到 `gait_busy` 错误而不是排队。

### 连续量

需**持续发送**来喂看门狗，建议 20 Hz。停发后 300 ms 内轴指令自动清零。
取值全部是归一化的 `[-1, 1]`。

```json
{"t":"vel","vx":0.5,"vy":0.0,"wz":0.2}      // 仅踏步态有效
{"t":"pose","h":0.0,"roll":0.0,"pitch":0.3,"yaw":0.0}  // 仅力控站立态有效
{"t":"release"}                              // 立即清零
```

`vx` 前正后负，`vy` 左正右负，`wz` 逆时针为正。
实际物理速度 = 归一化值 × 当前步态上限，上限见 `state.limits`。

### 保活

```json
{"t":"ping","id":42}
```

### 视频

**这一组不需要控制权**：围观视频是正常需求，不该和操控权绑定。
带宽约束由服务端单独强制，与控制权是两套机制。

上报本机解码能力。连上后应尽早发一次；服务端在收到之前按「只支持 H.264」
处理，因为 H.265 在 WebRTC 里**没有软解兜底**，猜错的代价是黑屏。

```json
{"t":"media_caps","h264":true,"h265":false}
```

必须**实测**再上报，不要按 UserAgent 推断——同一个 Chrome 版本在不同 SoC
上结论可能不同。遥控端的探测实现见 `web/media.js` 的 `probeCodecsSync()`。

选择主视图。全码率同一时刻只有一份，抢不到时会收到 `media_degraded`
并在计划里标出原因。

```json
{"t":"media_select","id":"ptz_vis"}
```

云台（需要控制权）。App 拨动开关向下时摇杆走这一路，不再发给狗。
各轴 -1..1：`pan` 右为正，`tilt` 上为正，`zoom` 拉近为正。

```json
{"t":"ptz","pan":0.4,"tilt":-0.2,"zoom":0}
```

主动索取一次当前计划（一般不需要，服务端会在相关变化时主动推）。

```json
{"t":"media_plan"}
```

### 点云

同样**不需要控制权**。

```json
{"t":"cloud_sub"}      // 开始接收
{"t":"cloud_unsub"}    // 停止接收
```

订阅是**按需的**：没有任何客户端订阅时，网关根本不去连感知主机的 ROS。
所以退订不是可有可无的礼貌——不退订就一直占着 MESH 带宽，
现场表现为视频莫名其妙变卡，而没人会想到是点云没停。
遥控端切后台时也应当退订。

未启用点云时返回 `no_cloud` 错误。

### 网关配置

读写网关自己的运行参数：运动主机、感知主机、监听地址、点云开关等。
控制台的「设置」面板走的就是这两条。

**不看控制权，看设置密码。** 操控机器狗和改网关指向是两回事，后者危险得多——
能把服务指到另一台主机上，也能把监听面从内网扩到全部网卡。而本协议
[没有身份认证](#没有身份认证)，所以这一类操作单设一道门。
密码是 `54longqr`（字段名仍是 `token`，也认 `password`）。

```json
{"t":"config_get","token":"54longqr"}
{"t":"config_set","token":"54longqr","settings":{"perception_ip":"192.168.1.205"}}
```

`settings` 里**只放要改的字段**，其余保持原值。可改的键与
`GatewaySettings` 一一对应：

| 键 | 类型 | 说明 |
| --- | --- | --- |
| `robot_ip` / `robot_port` | 字符串 / 数字 | 运动主机 |
| `local_port` | 数字 | 本机收遥测的端口，要与运动主机 `network.toml` 登记的一致 |
| `perception_ip` / `perception_port` | 字符串 / 数字 | 感知主机地形图通道 |
| `http_port` / `bind_address` | 数字 / 字符串 | 本服务自己的监听端口与地址 |
| `cloud_enabled` | 布尔 | 点云开关 |
| `ros_master` / `ros_host` / `cloud_topic` | 字符串 | 点云的 ROS 参数 |
| `cloud_hz` / `cloud_points` | 数字 | 点云下行帧率与单帧点数上限 |
| `ptz_vis_rtsp` / `ptz_ir_rtsp` | 字符串 | 双光布控球白光 / 热成像 RTSP。可空。口令写在地址里，云台从白光地址取主机 |
| `ptz_vis_codec` / `ptz_ir_codec` | 字符串 | `h264` 或 `h265`。可空：路径含 `/h264`、`/h265` 时按地址猜 |

刻意**不含** `--web` / `--media` 这类文件路径：那些是装机时定的部署布局，
从一个无 TLS 的网页去改服务端路径只会开出一条目录穿越的口子。

三件事会让 `config_set` 被拒，都是有意为之：

- **有人正持有控制权** → `busy_control`。改配置要重启网关，遥控会中断一两秒，
  狗正走着的时候不能发生。让对方先释放。
- **监听地址不是本机任何一块网卡的地址** → `bad_config`。这条最要紧：
  配错了重启后 `bind` 失败，服务起不来，控制台跟着消失，就再没有地方能改回来。
  同理，开点云时 `ros_host` 也必须是本机地址。
- **类型不对或键名拼错** → `bad_config`。不静默忽略，否则表现是
  「改了、保存了、没生效、也没报错」。

校验不通过时**一个字节都不会落盘**，不存在半份配置。

## 服务端 → 客户端

### 连接问候

连上立即下发一次。

```json
{"t":"hello","version":"0.2.3","client_id":3,"control":false,"lease_ms":2000,
 "config":true,"pose_adopt":true}
```

`config` 是个能力位：本机是否支持在线改配置（网关要以 `--config` 启动）。
它不含任何配置内容，遥控端只用它决定要不要显示「设置」入口；
真要取值仍须凭令牌。

`pose_adopt` 也是能力位：认不认 `claim.standing`。旧网关没有这个键。
遥控端据此判断该不该提示「请在 RK3588 上重装网关」，不再靠交接超时去猜。

### 控制权变更

```json
{"t":"control","granted":true,"holder":3}
```

### 步态切换结果

回应 `{"t":"cmd","name":"gait"}`，成功失败都回，只发给发起方。

```json
{"t":"gait_result","gait_key":"stair","ok":false,
 "code":"gait_not_applied","msg":"切换到楼梯未生效。楼梯步态需要……"}
```

| `code` | 含义 |
| --- | --- |
| `""` | 成功 |
| `no_telemetry` | 与运动主机失联，无法确认结果 |
| `not_stepping` | 真趴着（遥测坐下且网关不记得 RL 起立）。站着即可切 |
| `not_standstill` | 多帧楼梯要求静止，机器狗还在动 |
| `gait_not_applied` | 指令发了但遥测没确认。楼梯多半是地形图没配合上 |

`msg` 是面向操作员的完整说明，可直接展示。`gait_not_applied` 在楼梯步态下
会带上感知主机地址和排查方向——这类失败在实机上是静默的，
运动主机不回任何错误，光看按钮完全看不出来。

### 状态推送

默认 10 Hz。`alive` 为 false 表示 RK3588 与运动主机之间断了，
此时其余字段是最后一次已知值，遥控端应当明确置灰而不是继续显示旧数据。

```json
{
  "t": "state",
  "alive": true,
  "basic_state": 4,
  "basic_state_text": "踏步",
  "gait": 0,
  "gait_key": "walk",
  "gait_text": "Walk",
  "mode": 0,
  "height_gear": 0,
  "odom": {"x": 1.65, "y": 0.62, "yaw": 0.72},
  "vel": {"x": 0.60, "y": 0.0, "yaw": 0.24},
  "att": {"roll": -0.1, "pitch": -0.8, "yaw": 41.2},
  "battery": {"level": 80, "voltage": 72.0},
  "temp": {"cpu": 52.0, "motor_max": 44.2},
  "limits": {"forward": 1.2, "lateral": 0.8, "yaw": 1.2},
  "mileage_cm": 165,
  "errors": ["电机过温"],
  "emergency_source": 0
}
```

姿态角 `att` 单位是度，里程计 `odom.yaw` 单位是弧度（直接来自运动主机，不做转换）。

`gait_text` 是给人看的中文名，会变；判断当前步态请一律用 `gait_key`，
它与 `cmd/gait` 的 `value` 取值一致。

### 媒体计划

告诉遥控端「现在该拉哪几路、各自什么质量」。视频本身不经过网关，
遥控端拿着 `webrtc_base` 和 `path` 直接去 MediaMTX 拉。

连接后立即下发一次，之后在 `media_caps`、`media_select`、
以及**占着全码率槽位的客户端断开**时重新下发。

```json
{
  "t": "media_plan",
  "main": "ptz_vis",
  "webrtc_base": "http://192.168.10.2:8889",
  "budget_kbps": 3800,
  "sources": [
    {"id":"ptz_vis","name":"布控球可见光","audio":true,"ptz":true,
     "available":true,"path":"ptz_vis_main","codec":"h265","kbps":3000,
     "label":"1080p25","quality":"main"},
    {"id":"ptz_ir","name":"布控球热成像","audio":false,"ptz":true,
     "available":true,"path":"ptz_ir_sub","codec":"h264","kbps":400,
     "label":"480p10","quality":"thumb"}
  ],
  "total_kbps": 3400,
  "over_budget": false
}
```

拉流地址是 `{webrtc_base}/{path}/whep`（WHEP：把 SDP offer 以
`application/sdp` POST 过去，回来就是 answer）。

`quality` 为 `main` 表示全码率，`thumb` 表示缩略图。
**同一时刻全局只有一路 `main`**，这是 MESH 带宽决定的硬约束。

`available` 为 false 表示遥控端解不了这个源的任何一路，此时没有 `path`，
`reason` 说明原因。

选中的源没拿到全码率时会带 `downgraded: true` 和 `reason`。
两种原因要区分开：**编码不支持**（退到了子码流）和**槽位被占**
（别人正在看全码率）。默默给一张糊图是最差的处理，现场会误判成相机坏了。

### 点云状态

1 Hz 广播，订阅与退订时也立即回一次。

```json
{
  "t": "cloud_status",
  "active": true,
  "connected": true,
  "subscribers": 1,
  "points": 15076,
  "voxel": 0.1,
  "sent": 42,
  "dropped": 0,
  "error": "/lidar_points: 连不上 ROS master (http://192.168.1.105:11311)"
}
```

`connected` 指与感知主机 ROS 的连接，与 `active`（有没有人订阅）是两回事。
`error` 只在异常时出现，要原样显示给操作员——**感知主机的可达性是这条链路
最大的不确定性**，出问题时得让人一眼看出是感知主机的事，而不是以为整台车坏了。

`dropped` 持续增长说明链路发不动，此时该调低 `--cloud-hz` 或 `--cloud-points`。

### 点云数据帧

点云走 **WebSocket 二进制帧**，不是 JSON。同样的点数，二进制比 JSON 小一个
数量级，而 MESH 只有十几 Mbps——用 JSON 传点云是不可能的。

小端，头部 40 字节：

| 偏移 | 长度 | 字段 |
| --- | --- | --- |
| 0 | 4 | 魔数 `"X30C"` |
| 4 | 1 | 版本，当前为 1 |
| 5 | 1 | 标志位。bit0=1 表示世界系（LIO `/cloud_registered`），0 为机体系 |
| 6 | 2 | 保留 |
| 8 | 4 | 序号 |
| 12 | 8 | 时间戳（毫秒） |
| 20 | 12 | 包围盒原点 x, y, z（float32） |
| 32 | 4 | 量化步长 scale（float32，单位米） |
| 36 | 4 | 点数 count |
| 40 | count×6 | 每点三个 uint16 |

还原：`真实坐标 = 原点 + uint16 值 × scale`。

坐标量化成 uint16 是为了省一半带宽。以 ±40 m 的包围盒算，分辨率约 1.2 mm，
远小于雷达本身的精度，等于白拿。遥控端可以把 uint16 直接喂给 WebGL
（`vertexAttribPointer` 的 `normalized=true`），连转换都不用做，
实现见 `web/cloud.js`。

LIO 在发 `/cloud_registered` 时，下行是**世界系当前扫描**（flags bit0），
和 `/lio_odom` 同一套坐标，遥控端用当前位姿变到机体系显示，轨迹才能对上。
LIO 未就绪时仍下机体 `/lidar_points`。格式不变。

### 错误

```json
{"t":"error","code":"no_control","msg":"未持有控制权"}
```

`code` 取值：`no_control`、`bad_request`、`unknown_command`、
`gait_busy`、`no_media`、`media_degraded`、`no_cloud`、
`no_config`、`bad_admin_token`、`bad_config`、
`busy_control`、`config_write_failed`。

`media_degraded` 不是失败：视频照样能看，只是质量降了，
配合 `media_plan` 里的 `reason` 使用。

### 配置回执

`config_get` 的回应：

```json
{"t":"config","settings":{"robot_ip":"192.168.1.103","http_port":8080,"…":"…"},
 "path":"/opt/x30/conf/gateway.conf","auto_restart":true,"control_held":false}
```

`settings` 是**当前实际生效的**那一份，不是配置文件的内容——命令行参数会盖过
文件，回显文件就会和真正在跑的东西不一致。

`auto_restart` 表示网关能不能自己重启：由 systemd 托管时为 `true`
（写完配置就干净退出，`Restart=always` 一秒内把它带着新配置拉回来）；
手工在终端里跑时为 `false`，那时网关**不会**自己退出，需要人去重启——
退了就没人拉它回来了。

`config_set` 成功后的回应，字段含义同上：

```json
{"t":"config_saved","settings":{"…":"…"},"auto_restart":true}
```

`auto_restart` 为 `true` 时，回执发出约 400 ms 后连接会断开，
遥控端按正常重连逻辑等它回来即可。**如果改的是 `http_port` 或 `bind_address`，
连接不会自己回来**——遥控端要把新地址明确告诉操作员，否则界面上只会剩下
一个永远在「重连中」的状态。

### 保活应答

```json
{"t":"pong","id":42}
```

## 安全设计

三层兜底，缺一不可：

1. **遥控端停发** → `MotionClient` 看门狗 300 ms 内把轴指令清零，机器人减速停下。
2. **遥控端断连** → 立即释放控制权并清零，不等租约超时。
3. **RK3588 宕机** → 心跳中断，机器人按 `overtime.toml` 自行降级
   （`StopMotion` → `KeepStill` → `Sitdown`）。

网关的看门狗超时应当设得比机器人的 `StopMotion` 更短，
这样正常情况下永远轮不到机器人自己降级。

### 没有身份认证

**本协议目前不做身份认证。** 凡是能建立 WebSocket 连接的客户端都能申请控制权，
进而驱动机器狗。上面三层兜底防的是链路故障，不是恶意接入。

唯一的例外是 [`config_get` / `config_set`](#网关配置)：它们要设置密码。
不是因为那里做了认证，而是因为改配置能把网关指到别处、也能把监听面自己打开，
放任不管等于让上面这条限制形同虚设。**其余所有消息一律没有身份校验。**

这在隔离的遥控局域网里是可接受的，但**一旦本机接入 4G、公网或不受控的 WiFi
就不成立**。在补上认证之前，靠部署收敛暴露面：

- 用 `--bind <地址>` 把服务限定在遥控链路所在的网卡（默认 `0.0.0.0` 会监听全部
  网卡，启动时有警告）。
- 用防火墙只放行遥控链路上的 8080。
- 需要远程访问就走 WireGuard 之类的隧道，不要把端口直接暴露到公网。

部署做法见 [rk3588-setup.md 第 18 节](rk3588-setup.md)，
`tools/bind_check.sh` 可以验证限制确实生效。
