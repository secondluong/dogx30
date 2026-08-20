// 网关的可改配置：一份 key = value 文件，控制台「设置」面板直接读写它。
//
// 为什么要有这个文件：这些地址以前只存在于 systemd 单元的 ExecStart 里，
// 装完就只能上命令行改。现场换一台感知主机、或者发现板子被路由器 NAT 到了
// 别的网段需要改绑定地址时，人蹲在狗旁边用 vi 编辑 systemd 单元是不现实的。
//
// 单一来源：单元文件只传 --config，不再重复这些参数。两处各存一份的话，
// 迟早出现「单元里写着 .103、实际跑的是 .200」，而去读单元的人会信单元 ——
// 这和 render_unit.sh 当初要消灭的那三份 sed 副本是同一类毛病。

#pragma once

#include <cstdint>
#include <string>

namespace x30 {

class Json;

// 可在线修改的参数全集。刻意不含 --web / --media / --prefix 这类文件路径：
// 那些是装机时定的部署布局，从一个无 TLS 的网页去改服务端路径只会开出
// 一条目录穿越的口子，收益却近乎零。
struct GatewaySettings {
  std::string robot_ip = "192.168.1.103";
  uint16_t robot_port = 43893;
  uint16_t local_port = 43897;

  std::string perception_ip = "192.168.1.105";
  uint16_t perception_port = 43899;

  uint16_t http_port = 8080;
  std::string bind_address = "0.0.0.0";

  bool cloud_enabled = false;
  std::string ros_master = "http://192.168.1.105:11311";
  std::string ros_host = "192.168.1.120";
  std::string cloud_topic = "/lidar_points";
  int cloud_hz = 2;
  uint32_t cloud_points = 20000;

  // 双光布控球。空 = 沿用 mediamtx.yml / media.json 里现成的。
  // 填了就用这两条 RTSP 拉流，并从地址里取出主机和口令做云台。
  std::string ptz_vis_rtsp;
  std::string ptz_ir_rtsp;
  // h264 / h265。空 = 从 RTSP 路径猜，再猜不到就用 media.json。
  std::string ptz_vis_codec;
  std::string ptz_ir_codec;
};

enum class ConfigLoad {
  kOk,
  kMissing,    // 文件不存在。首次启动的正常情况，用默认值即可。
  kMalformed,  // 文件在但读不懂。这个必须让启动失败，不能"尽力而为"地
               // 退回默认值 —— 那会让网关连到一台操作员没想到的主机上。
};

ConfigLoad LoadGatewaySettings(const std::string& path, GatewaySettings* out,
                               std::string* error);

// 原子写：先写同目录临时文件，fsync 后再 rename。板子是直接断电关机的，
// 半份配置文件会让下次开机起不来。
bool SaveGatewaySettings(const std::string& path, const GatewaySettings& s,
                         std::string* error);

// 把遥控端发来的 settings 对象叠加到 inout 上。只认已知键，类型不对就报错
// 而不是静默忽略 —— 静默忽略的表现是「点了保存、值没变、也没报错」。
// 未提供的字段保持原值，遥控端可以只发改动的那几项。
bool MergeGatewaySettings(const Json& obj, GatewaySettings* inout,
                          std::string* error);

std::string GatewaySettingsJson(const GatewaySettings& s);

// 写文件之前的校验。拦的是会把操作员锁在外面、或让服务陷入重启循环的值：
// 配错之后网关起不来，控制台随之消失，就只能上命令行救了。
//
// current 是当前生效的配置，用来判断监听地址/端口有没有变 ——
// 没变就不去试探绑定，那个端口正被自己占着。
bool ValidateGatewaySettings(const GatewaySettings& s,
                             const GatewaySettings& current,
                             std::string* error);

// ip 是否为本机某块网卡的地址。--bind 和 --ros-host 都要求如此，
// 前者填错的症状是服务起不来，后者是订阅成功却一帧数据都收不到。
bool IsLocalIpv4(const std::string& ip);

// 控制台「设置」的密码。现场一台狗、内网用，不再用每机一份的令牌文件。
// --admin-token-file 仍可写在单元里（旧安装），网关不再读它。
inline constexpr const char* kAdminPassword = "54longqr";

std::string LoadAdminToken(const std::string& path);

// 定长时间比较，不因首字节不同就提前返回。
bool TokenMatches(const std::string& expected, const std::string& given);

// 从 rtsp://user:pass@host:554/path 取出主机和口令，给海康 ISAPI 云台用。
bool ParseRtspAuthority(const std::string& url, std::string* host,
                        std::string* user, std::string* password,
                        std::string* error);

// media.json 旁边的 mediamtx.yml。
std::string MediamtxPathBeside(const std::string& media_json);

// 读 / 写 MediaMTX 某个 path 的 source。海康主码流 101/201 会顺带改子码流 102/202。
std::string ReadMediamtxSource(const std::string& yml_path,
                               const std::string& path_name);
bool ApplyPtzRtspToMediamtx(const std::string& yml_path,
                            const GatewaySettings& s, std::string* error,
                            bool* wrote = nullptr);

// 路径里带 /h264、/h265、/hevc 时认出来；海康 Channels/101 这种认不出来。
std::string InferRtspCodec(const std::string& url);

// 设置里写了用设置的；否则从地址猜。仍为空表示别动 media.json。
std::string EffectivePtzCodec(const std::string& configured,
                              const std::string& rtsp_url);

}  // namespace x30
