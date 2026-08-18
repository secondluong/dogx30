#include "x30/cloud_bridge.hpp"

#include <chrono>
#include <cstdio>

#include "x30/json.hpp"

namespace x30 {
namespace {

uint64_t NowMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace

CloudBridge::CloudBridge(CloudBridgeConfig config, WsServer& server)
    : cfg_(std::move(config)), server_(server), encoder_(cfg_.cloud) {}

CloudBridge::~CloudBridge() { Stop(); }

void CloudBridge::Start() {
  if (running_.exchange(true)) return;
  supervisor_ = std::thread(&CloudBridge::Supervisor, this);
}

void CloudBridge::Stop() {
  if (!running_.exchange(false)) return;
  wake_.notify_all();
  if (supervisor_.joinable()) supervisor_.join();
  StopRos();
}

void CloudBridge::AddSubscriber(WsServer::ClientId id) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_.insert(id);
  }
  wake_.notify_all();
}

void CloudBridge::RemoveSubscriber(WsServer::ClientId id) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_.erase(id);
  }
  wake_.notify_all();
}

void CloudBridge::StartRos() {
  if (ros_active_.load()) return;

  RosClientConfig rc;
  rc.master_uri = cfg_.master_uri;
  rc.node_host = cfg_.node_host;
  rc.node_name = "/x30_gateway";

  // 每次都重建，不复用停过的实例：从节点每次绑的是随机端口，
  // 重建比在 RosClient 里维护可重启状态更不容易出错。
  ros_.reset(new RosClient(rc));
  ros_->Subscribe({cfg_.topic, "sensor_msgs/PointCloud2",
                   ros_md5::kPointCloud2},
                  [this](const uint8_t* d, size_t n) { OnCloudMessage(d, n); });

  std::string err;
  if (!ros_->Start(&err)) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = err;
    ros_.reset();
    return;
  }
  ros_active_.store(true);
  std::printf("[cloud] 已启动点云订阅 %s\n", cfg_.topic.c_str());
}

void CloudBridge::StopRos() {
  if (!ros_active_.exchange(false)) return;
  if (ros_) {
    ros_->Stop();
    ros_.reset();
  }
  std::printf("[cloud] 无人订阅，已断开点云\n");
}

void CloudBridge::Supervisor() {
  while (running_.load()) {
    bool want = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      want = !subscribers_.empty();
    }

    if (want && !ros_active_.load()) {
      StartRos();
    } else if (!want && ros_active_.load()) {
      StopRos();
    }

    // ROS 侧的错误抄到本地，供遥测展示。可达性是这条链路最大的不确定性，
    // 出问题时要能在界面上直接看出卡在哪一步，而不是去翻板子日志。
    if (ros_active_.load() && ros_) {
      const std::string e = ros_->last_error();
      std::lock_guard<std::mutex> lock(mutex_);
      last_error_ = ros_->connected() ? std::string() : e;
    }

    std::unique_lock<std::mutex> lock(wake_mutex_);
    wake_.wait_for(lock, std::chrono::milliseconds(500),
                   [this] { return !running_.load(); });
  }
}

void CloudBridge::OnCloudMessage(const uint8_t* data, size_t len) {
  std::vector<WsServer::ClientId> targets;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (subscribers_.empty()) return;
    targets.assign(subscribers_.begin(), subscribers_.end());
  }

  // 抽帧放在解析之前：话题是 10 Hz，下行只要 2 Hz，
  // 剩下 8 帧连解析都不该做。
  if (!encoder_.ShouldEmit(NowMs())) return;

  std::string err;
  if (!ParsePointCloud2(data, len, &parse_buffer_, &err)) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = "点云解析失败: " + err;
    return;
  }

  encoder_.Encode(parse_buffer_, &encode_buffer_);

  size_t sent = 0, dropped = 0;
  for (const auto id : targets) {
    if (server_.SendBinary(id, encode_buffer_.data(), encode_buffer_.size())) {
      ++sent;
    } else {
      // 发不出去多半是链路拥塞触发了发送超时。丢掉就是了 ——
      // 迟到的点云没有价值，堆积只会让延迟越滚越大。
      ++dropped;
    }
  }

  // 编码器的统计量只在这个线程写，抄一份到锁内，避免 StatusJson 读到撕裂值。
  std::lock_guard<std::mutex> lock(mutex_);
  frames_sent_ += sent;
  frames_dropped_ += dropped;
  last_points_ = encoder_.last_point_count();
  last_voxel_ = encoder_.effective_voxel();
  if (!last_error_.empty()) last_error_.clear();
}

std::string CloudBridge::StatusJson() const {
  std::lock_guard<std::mutex> lock(mutex_);
  JsonWriter w;
  w.BeginObject();
  w.Key("t", "cloud_status");
  w.Key("active", !subscribers_.empty());
  w.Key("connected", ros_ ? ros_->connected() : false);
  w.Key("subscribers", static_cast<double>(subscribers_.size()));
  w.Key("points", static_cast<double>(last_points_));
  w.Key("voxel", static_cast<double>(last_voxel_));
  w.Key("sent", static_cast<double>(frames_sent_));
  w.Key("dropped", static_cast<double>(frames_dropped_));
  if (!last_error_.empty()) w.Key("error", last_error_);
  w.EndObject();
  return w.Take();
}

}  // namespace x30
