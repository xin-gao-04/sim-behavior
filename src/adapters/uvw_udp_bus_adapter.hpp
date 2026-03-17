#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// UvwUdpBusAdapter
//
// IBusAdapter 的 uvw/libuv UDP 实现（Integration Adapters 第六层）。
//
// 工作模型：
//   - 绑定一个本地 UDP 端口（recv），同时向固定 remote_host:remote_port 发送。
//   - Connect() / Disconnect() / Publish() 必须在 uvw loop 线程上调用
//     （通过 IEventLoopRuntime::PostToLoop 进入）。
//   - Subscribe() 可在任意线程调用（仅写 handlers_ map，用 mutex 保护）。
//   - 收到 UDP 数据包时在 loop 线程执行 Dispatch，查找匹配 topic 的 handler。
//
// 帧格式（大端序）：
//   [4B magic: 0x42 0x55 0x53 0x21 "BUS!"]
//   [2B topic_len][topic_len bytes topic]
//   [8B timestamp_ms LE]
//   [4B payload_len LE][payload_len bytes payload]
// ─────────────────────────────────────────────────────────────────────────────

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <uvw.hpp>

#include "sim_bt/runtime/async_runtime/i_bus_adapter.hpp"

namespace sim_bt {

class UvwUdpBusAdapter : public IBusAdapter {
 public:
  // loop       : uvw loop 引用（UvwEventLoopRuntime::Loop()），
  //              必须在 loop 存活期间使用此 adapter。
  // local_port : 绑定本地端口（0 = 随机端口，由 OS 分配）。
  // remote_host / remote_port : Publish() 默认目标。
  UvwUdpBusAdapter(uvw::loop& loop,
                   uint16_t   local_port,
                   std::string remote_host,
                   uint16_t   remote_port)
      : loop_(loop),
        local_port_(local_port),
        remote_host_(std::move(remote_host)),
        remote_port_(remote_port) {}

  ~UvwUdpBusAdapter() override {
    if (connected_) Disconnect();
  }

  const char* Name() const override { return "UvwUdpBusAdapter"; }

  // 必须从 loop 线程调用（通过 PostToLoop 投递）。
  SimStatus Connect() override {
    if (connected_) return SimStatus::Ok();
    udp_ = loop_.resource<uvw::udp_handle>();
    if (!udp_) {
      return SimStatus::Err(1, "UvwUdpBusAdapter: failed to create udp_handle");
    }

    udp_->on<uvw::udp_data_event>(
        [this](const uvw::udp_data_event& ev, uvw::udp_handle&) {
          OnData(ev.data.get(), ev.length);
        });
    udp_->on<uvw::error_event>(
        [](const uvw::error_event& ev, uvw::udp_handle&) {
          (void)ev;  // error ignored in header-only implementation
        });

    if (udp_->bind("0.0.0.0", local_port_) < 0) {
      return SimStatus::Err(2, "UvwUdpBusAdapter: bind failed");
    }

    // 更新实际绑定端口（local_port_=0 时 OS 分配）
    auto sa = udp_->sock();
    bound_port_ = sa.port;

    udp_->recv();
    connected_ = true;
    // connected successfully
    return SimStatus::Ok();
  }

  // 必须从 loop 线程调用。
  void Disconnect() override {
    if (!connected_) return;
    if (udp_) {
      udp_->close();
      udp_.reset();
    }
    connected_ = false;
    // disconnected
  }

  bool IsConnected() const override { return connected_; }

  // 线程安全：可在任意线程调用。
  SimStatus Subscribe(const std::string& topic,
                      BusMessageCallback callback) override {
    std::lock_guard<std::mutex> lk(handlers_mu_);
    handlers_[topic] = std::move(callback);
    return SimStatus::Ok();
  }

  // 线程安全：可在任意线程调用。
  void Unsubscribe(const std::string& topic) override {
    std::lock_guard<std::mutex> lk(handlers_mu_);
    handlers_.erase(topic);
  }

  // 必须从 loop 线程调用（通过 PostToLoop 投递）。
  SimStatus Publish(const BusMessage& message) override {
    if (!connected_ || !udp_) {
      return SimStatus::Err(3, "UvwUdpBusAdapter: not connected");
    }

    auto frame = Serialize(message);
    auto len   = static_cast<unsigned int>(frame.size());
    auto data  = std::make_unique<char[]>(len);
    std::memcpy(data.get(), frame.data(), len);

    udp_->send(remote_host_,
               static_cast<unsigned int>(remote_port_),
               std::move(data), len);
    return SimStatus::Ok();
  }

  // 实际绑定端口（local_port_=0 时由 OS 分配，Connect() 后可读）。
  uint16_t BoundPort() const { return bound_port_; }

  // 修改发送目标（允许在 Connect() 后调整，需在 loop 线程调用）。
  void SetRemote(const std::string& host, uint16_t port) {
    remote_host_ = host;
    remote_port_ = port;
  }

 private:
  // ── 帧序列化 ─────────────────────────────────────────────────────────────

  static constexpr uint32_t kMagic = 0x42555321u;  // "BUS!"

  static std::vector<uint8_t> Serialize(const BusMessage& msg) {
    const uint16_t tlen = static_cast<uint16_t>(msg.topic.size());
    const uint32_t plen = static_cast<uint32_t>(msg.payload.size());
    // magic(4) + tlen(2) + topic + ts(8) + plen(4) + payload
    size_t total = 4 + 2 + tlen + 8 + 4 + plen;
    std::vector<uint8_t> buf(total);
    uint8_t* p = buf.data();

    // magic
    auto write32 = [](uint8_t*& dst, uint32_t v) {
      dst[0] = (v      ) & 0xFF;
      dst[1] = (v >>  8) & 0xFF;
      dst[2] = (v >> 16) & 0xFF;
      dst[3] = (v >> 24) & 0xFF;
      dst += 4;
    };
    auto write16 = [](uint8_t*& dst, uint16_t v) {
      dst[0] = (v     ) & 0xFF;
      dst[1] = (v >> 8) & 0xFF;
      dst += 2;
    };
    auto write64 = [](uint8_t*& dst, uint64_t v) {
      for (int i = 0; i < 8; ++i) { dst[i] = (v >> (i * 8)) & 0xFF; }
      dst += 8;
    };

    write32(p, kMagic);
    write16(p, tlen);
    std::memcpy(p, msg.topic.data(), tlen); p += tlen;
    write64(p, msg.timestamp_ms);
    write32(p, plen);
    if (plen > 0) std::memcpy(p, msg.payload.data(), plen);
    return buf;
  }

  static bool Deserialize(const char* raw, size_t len, BusMessage& out) {
    if (len < 4 + 2 + 8 + 4) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(raw);

    auto read32 = [](const uint8_t*& src) -> uint32_t {
      uint32_t v = static_cast<uint32_t>(src[0])
                 | (static_cast<uint32_t>(src[1]) <<  8)
                 | (static_cast<uint32_t>(src[2]) << 16)
                 | (static_cast<uint32_t>(src[3]) << 24);
      src += 4; return v;
    };
    auto read16 = [](const uint8_t*& src) -> uint16_t {
      uint16_t v = static_cast<uint16_t>(src[0])
                 | (static_cast<uint16_t>(src[1]) << 8);
      src += 2; return v;
    };
    auto read64 = [](const uint8_t*& src) -> uint64_t {
      uint64_t v = 0;
      for (int i = 0; i < 8; ++i) v |= (static_cast<uint64_t>(src[i]) << (i * 8));
      src += 8; return v;
    };

    uint32_t magic = read32(p);
    if (magic != kMagic) return false;

    uint16_t tlen = read16(p);
    if (static_cast<size_t>(p - reinterpret_cast<const uint8_t*>(raw)) + tlen + 8 + 4 > len)
      return false;

    out.topic.assign(reinterpret_cast<const char*>(p), tlen); p += tlen;
    out.timestamp_ms = read64(p);

    uint32_t plen = read32(p);
    if (static_cast<size_t>(p - reinterpret_cast<const uint8_t*>(raw)) + plen > len)
      return false;

    out.payload.assign(p, p + plen);
    return true;
  }

  void OnData(const char* data, size_t length) {
    BusMessage msg;
    if (!Deserialize(data, length, msg)) {
      return;  // malformed frame, ignored
      return;
    }
    // 查找 handler（持锁拷贝，避免回调期间死锁）
    BusMessageCallback cb;
    {
      std::lock_guard<std::mutex> lk(handlers_mu_);
      auto it = handlers_.find(msg.topic);
      if (it == handlers_.end()) return;
      cb = it->second;
    }
    cb(msg);
  }

  uvw::loop& loop_;
  uint16_t   local_port_;
  uint16_t   bound_port_ = 0;
  std::string remote_host_;
  uint16_t    remote_port_;

  std::shared_ptr<uvw::udp_handle> udp_;
  bool connected_ = false;

  std::mutex handlers_mu_;
  std::unordered_map<std::string, BusMessageCallback> handlers_;
};

}  // namespace sim_bt
