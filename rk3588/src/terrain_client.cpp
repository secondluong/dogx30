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

HeightMapMode TerrainClient::last_mode() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_mode_;
}

}  // namespace x30
