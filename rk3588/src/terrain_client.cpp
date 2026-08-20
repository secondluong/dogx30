#include "x30/terrain_client.hpp"

#include <cstdio>

namespace x30 {

TerrainClient::TerrainClient(TerrainClientConfig config)
    : cfg_(std::move(config)) {}

TerrainClient::~TerrainClient() { Stop(); }

bool TerrainClient::Start(std::string* error) {
  if (running_.load()) return true;

  // 本机端口由系统分配：这条通道只写，不需要固定端口来接收单播。
  if (!tx_.Open(0, error)) return false;
  if (!tx_.SetPeer(cfg_.perception_ip, cfg_.perception_port, error)) return false;

  running_.store(true);
  return true;
}

void TerrainClient::Stop() {
  if (!running_.exchange(false)) return;
  tx_.Close();
}

bool TerrainClient::SendSimple(uint32_t code, uint32_t value) {
  if (!running_.load()) return false;
  CommandHead head{};
  head.code = code;
  head.paramters_size = value;
  head.type = kTypeSimple;
  return tx_.Send(&head, sizeof(head));
}

bool TerrainClient::SetHeightMapMode(HeightMapMode mode) {
  if (!SendSimple(terrain::kHeightMapMode, static_cast<uint32_t>(mode))) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    last_mode_ = mode;
  }
  has_sent_mode_.store(true);
  std::printf("[地形图] 模式 -> %s\n", ToString(mode));
  return true;
}

bool TerrainClient::SetStepZMax(StepZMax v) {
  if (!SendSimple(terrain::kStepZMax, static_cast<uint32_t>(v))) return false;
  std::printf("[地形图] 障碍高度阈值 -> %s\n",
              v == StepZMax::k28cm ? "28cm" : "8cm");
  return true;
}

bool TerrainClient::SetVelSource(VelSource v) {
  return SendSimple(terrain::kVelSource, static_cast<uint32_t>(v));
}

bool TerrainClient::SetBrakeMode(BrakeMode v) {
  return SendSimple(terrain::kBrakeMode, static_cast<uint32_t>(v));
}

bool TerrainClient::StartLio(bool on, std::string* error) {
  UdpEndpoint ep;
  std::string err;
  if (!ep.Open(0, &err)) {
    if (error) *error = err;
    return false;
  }
  if (!ep.SetPeer(cfg_.perception_ip, terrain::kLioPort, &err)) {
    if (error) *error = err;
    return false;
  }
  CommandHead head{};
  head.code = terrain::kLioToggle;
  head.paramters_size = on ? 1u : 0u;
  head.type = kTypeSimple;
  if (!ep.Send(&head, sizeof(head))) {
    if (error) *error = "LIO 指令发送失败";
    return false;
  }
  CommandHead reply{};
  const int n = ep.Recv(&reply, sizeof(reply), 800);
  if (n < static_cast<int>(sizeof(reply))) {
    if (error) *error = "感知主机 :60000 无应答";
    std::printf("[LIO] %s（%s:%u %s）\n", error ? error->c_str() : "无应答",
                cfg_.perception_ip.c_str(), terrain::kLioPort,
                on ? "开" : "关");
    return false;
  }
  // 文档：value 0=成功，-1=失败。uint32 里 -1 是全 F。
  if (reply.paramters_size != 0) {
    if (error) *error = "感知主机拒绝启动 LIO";
    std::printf("[LIO] 被拒绝 value=%u（狗要站稳，或先退多帧地形图）\n",
                reply.paramters_size);
    return false;
  }
  std::printf("[LIO] 原厂激光惯性里程计已%s\n", on ? "启动" : "关闭");
  return true;
}

HeightMapMode TerrainClient::last_mode() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_mode_;
}

}  // namespace x30
