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

// 管理令牌。文件缺失、读不了或内容为空时返回空串，此时改配置一律拒绝 ——
// 失败关闭。协议本身没有身份认证，不能让「凡能连上 8080 的人」都能改
// 运动主机地址，或者把 --bind 从内网地址改成 0.0.0.0 自己把口子开出来。
std::string LoadAdminToken(const std::string& path);

// 定长时间比较，不因首字节不同就提前返回。
bool TokenMatches(const std::string& expected, const std::string& given);

}  // namespace x30
