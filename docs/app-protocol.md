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
{"t":"claim"}      // 申请控制权
{"t":"yield"}      // 主动交还
```

### 离散指令

```json
{"t":"cmd","name":"stand"}                  // 坐 <-> 站 切换
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

## 服务端 → 客户端

### 连接问候

连上立即下发一次。

```json
{"t":"hello","version":"0.2.0","client_id":3,"control":false,"lease_ms":2000}
```

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
| `not_stepping` | 当前不是踏步态，只有踏步态能切步态 |
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
| 5 | 1 | 标志位，暂时全 0 |
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

当前下发的是**机体系当前帧**，不是累积地图——LIO 还没上。
等有了配准点云，换的只是网关订阅的话题，这个格式不变。

### 错误

```json
{"t":"error","code":"no_control","msg":"未持有控制权"}
```

`code` 取值：`no_control`、`bad_request`、`unknown_command`、
`gait_busy`、`no_media`、`media_degraded`、`no_cloud`。

`media_degraded` 不是失败：视频照样能看，只是质量降了，
配合 `media_plan` 里的 `reason` 使用。

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

这在隔离的遥控局域网里是可接受的，但**一旦本机接入 4G、公网或不受控的 WiFi
就不成立**。在补上认证之前，靠部署收敛暴露面：

- 用 `--bind <地址>` 把服务限定在遥控链路所在的网卡（默认 `0.0.0.0` 会监听全部
  网卡，启动时有警告）。
- 用防火墙只放行遥控链路上的 8080。
- 需要远程访问就走 WireGuard 之类的隧道，不要把端口直接暴露到公网。

部署做法见 [rk3588-setup.md 第 18 节](rk3588-setup.md)，
`tools/bind_check.sh` 可以验证限制确实生效。
