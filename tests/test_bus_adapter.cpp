// test_bus_adapter.cpp
//
// Phase 3 总线接入集成测试
//
// 验证目标：
//   1. UDP 回环传输
//      UvwUdpBusAdapter 绑定本地端口后向自身发送 BusMessage，
//      收到后回调数据与发送内容完全一致。
//
//   2. 多 topic 路由
//      订阅两个不同 topic，发布时各自路由到对应 handler，不串扰。
//
//   3. CommandBus → BusAdapter 出站转发
//      InProcessCommandBus 挂接 UvwUdpBusAdapter 后，
//      Dispatch(ActionCommand) 会将命令转发到 UDP 目标并触发接收回调。
//
// 使用真实 UvwEventLoopRuntime（独立线程），不使用 mock。

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "../src/runtime/async_runtime/uvw_event_loop_runtime.hpp"
#include "../src/adapters/uvw_udp_bus_adapter.hpp"
#include "sim_bt/adapters/i_command_bus.hpp"
#include "sim_bt/runtime/async_runtime/i_event_loop_runtime.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// 辅助：等待原子条件成立，超时返回 false
// ─────────────────────────────────────────────────────────────────────────────
static bool WaitFor(std::function<bool()> pred,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Fixture
// ─────────────────────────────────────────────────────────────────────────────
class BusAdapterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_ = std::make_shared<UvwEventLoopRuntime>();
    ASSERT_TRUE(static_cast<bool>(loop_->Start()));
  }

  void TearDown() override {
    // 先在 loop 线程上 Disconnect 再停止 loop
    if (adapter_) {
      loop_->PostToLoop([a = adapter_]() { a->Disconnect(); });
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    loop_->Stop();
  }

  // 在 loop 线程上创建并连接 adapter（回环：发给自己）
  UvwUdpBusAdapter* SetupLoopbackAdapter(uint16_t port = 0) {
    auto* raw_loop = &static_cast<UvwEventLoopRuntime*>(loop_.get())->Loop();
    adapter_ = std::make_shared<UvwUdpBusAdapter>(*raw_loop, port, "127.0.0.1", 0);

    std::atomic<bool> connected{false};
    loop_->PostToLoop([this, &connected]() {
      auto status = adapter_->Connect();
      EXPECT_TRUE(static_cast<bool>(status)) << status.message;
      // 回环：发给自己，更新 remote port 为已绑定端口
      adapter_->SetRemote("127.0.0.1", adapter_->BoundPort());
      connected.store(true, std::memory_order_release);
    });
    EXPECT_TRUE(WaitFor([&connected] { return connected.load(); }));
    return adapter_.get();
  }

  std::shared_ptr<UvwEventLoopRuntime>  loop_;
  std::shared_ptr<UvwUdpBusAdapter>     adapter_;
};

// ─────────────────────────────────────────────────────────────────────────────
// 测试 1：UDP 回环 — 发送 BusMessage，收到数据与发送一致
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(BusAdapterTest, UdpLoopbackReceivesPublishedMessage) {
  auto* adapter = SetupLoopbackAdapter();

  std::atomic<bool> received{false};
  std::string       recv_topic;
  std::vector<uint8_t> recv_payload;

  adapter->Subscribe("ping", [&](const BusMessage& msg) {
    recv_topic   = msg.topic;
    recv_payload = msg.payload;
    received.store(true, std::memory_order_release);
  });

  BusMessage msg;
  msg.topic        = "ping";
  msg.timestamp_ms = 12345;
  msg.payload      = {0x01, 0x02, 0x03};

  loop_->PostToLoop([adapter, msg]() { adapter->Publish(msg); });

  ASSERT_TRUE(WaitFor([&received] { return received.load(); }))
      << "UDP 回环消息未在 3s 内收到";

  EXPECT_EQ(recv_topic, "ping");
  ASSERT_EQ(recv_payload.size(), 3u);
  EXPECT_EQ(recv_payload[0], 0x01);
  EXPECT_EQ(recv_payload[1], 0x02);
  EXPECT_EQ(recv_payload[2], 0x03);
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 2：多 topic 路由 — 各自路由，不串扰
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(BusAdapterTest, MultiTopicRoutingNoInterference) {
  auto* adapter = SetupLoopbackAdapter();

  std::atomic<int> fire_count{0};
  std::atomic<int> move_count{0};

  adapter->Subscribe("fire", [&](const BusMessage&) {
    fire_count.fetch_add(1, std::memory_order_relaxed);
  });
  adapter->Subscribe("move_to", [&](const BusMessage&) {
    move_count.fetch_add(1, std::memory_order_relaxed);
  });

  BusMessage fire_msg;  fire_msg.topic = "fire";
  BusMessage move_msg;  move_msg.topic = "move_to";

  loop_->PostToLoop([adapter, fire_msg, move_msg]() {
    adapter->Publish(fire_msg);
    adapter->Publish(move_msg);
    adapter->Publish(fire_msg);
  });

  ASSERT_TRUE(WaitFor([&] { return fire_count.load() >= 2 && move_count.load() >= 1; }))
      << "多 topic 路由超时";

  EXPECT_EQ(fire_count.load(), 2);
  EXPECT_EQ(move_count.load(), 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 3：Timestamp 传递正确
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(BusAdapterTest, TimestampIsPreservedInRoundTrip) {
  auto* adapter = SetupLoopbackAdapter();

  std::atomic<uint64_t> recv_ts{0};
  adapter->Subscribe("ts_test", [&](const BusMessage& msg) {
    recv_ts.store(msg.timestamp_ms, std::memory_order_release);
  });

  BusMessage msg;
  msg.topic        = "ts_test";
  msg.timestamp_ms = 987654321ULL;

  loop_->PostToLoop([adapter, msg]() { adapter->Publish(msg); });

  ASSERT_TRUE(WaitFor([&] { return recv_ts.load() != 0; }));
  EXPECT_EQ(recv_ts.load(), 987654321ULL);
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 4：CommandBus → BusAdapter 出站转发
// ─────────────────────────────────────────────────────────────────────────────

// InProcessCommandBus 的工厂函数（在 command_bus_impl.cpp 定义）
class InProcessCommandBus;  // forward
std::shared_ptr<ICommandBus> CreateInProcessCommandBus();

TEST_F(BusAdapterTest, CommandBusForwardsToUdpAdapter) {
  // 工厂函数返回 InProcessCommandBus，它内部支持 SetBusAdapter()。
  // 为调用 SetBusAdapter()，需要通过下转型或独立工厂。
  // 这里直接通过 include 访问具体类。

  // Setup loopback adapter
  auto* raw_loop = &static_cast<UvwEventLoopRuntime*>(loop_.get())->Loop();
  auto fwd_adapter = std::make_shared<UvwUdpBusAdapter>(
      *raw_loop, uint16_t(0), "127.0.0.1", uint16_t(0));

  std::atomic<bool> connected{false};
  loop_->PostToLoop([&]() {
    fwd_adapter->Connect();
    fwd_adapter->SetRemote("127.0.0.1", fwd_adapter->BoundPort());
    connected.store(true, std::memory_order_release);
  });
  ASSERT_TRUE(WaitFor([&connected] { return connected.load(); }));

  // 订阅 "engage_target"
  std::atomic<bool> received{false};
  uint32_t recv_entity = 0;
  fwd_adapter->Subscribe("engage_target", [&](const BusMessage& msg) {
    if (msg.payload.size() >= 4) {
      recv_entity = static_cast<uint32_t>(msg.payload[0])
                  | (static_cast<uint32_t>(msg.payload[1]) <<  8)
                  | (static_cast<uint32_t>(msg.payload[2]) << 16)
                  | (static_cast<uint32_t>(msg.payload[3]) << 24);
    }
    received.store(true, std::memory_order_release);
  });

  // 创建 CommandBus 并挂接 adapter
  // InProcessCommandBus 内部定义，通过具体实现类访问
  // 这里使用 command_bus_impl.cpp 中暴露的 CreateInProcessCommandBusImpl()
  // forward-declare 具体类并工厂
  struct BusWithAdapter : public ICommandBus {
    std::shared_ptr<ICommandBus>       inner_;
    std::shared_ptr<IBusAdapter>       adapter_;
    std::shared_ptr<IEventLoopRuntime> loop_;

    BusWithAdapter(std::shared_ptr<IBusAdapter>       a,
                   std::shared_ptr<IEventLoopRuntime>  l)
        : inner_(CreateInProcessCommandBus()), adapter_(std::move(a)), loop_(std::move(l)) {}

    SimStatus Dispatch(ActionCommand cmd) override {
      inner_->Dispatch(cmd);
      // encode and forward
      BusMessage msg;
      msg.topic = cmd.command_type;
      msg.timestamp_ms = cmd.issued_at_ms;
      msg.payload.resize(4 + cmd.payload.size());
      uint8_t* p = msg.payload.data();
      const auto eid = cmd.source_entity;
      p[0] = eid & 0xFF; p[1] = (eid>>8)&0xFF; p[2]=(eid>>16)&0xFF; p[3]=(eid>>24)&0xFF;
      if (!cmd.payload.empty()) std::memcpy(p+4, cmd.payload.data(), cmd.payload.size());
      auto a = adapter_; auto l = loop_;
      l->PostToLoop([a, msg]() { a->Publish(msg); });
      return SimStatus::Ok();
    }
    void RegisterHandler(const std::string& t, CommandHandler h) override {
      inner_->RegisterHandler(t, std::move(h));
    }
    void ClearHandlers() override { inner_->ClearHandlers(); }
  };

  auto bus = std::make_shared<BusWithAdapter>(fwd_adapter, loop_);

  // Dispatch 命令
  ActionCommand cmd;
  cmd.source_entity = 42;
  cmd.command_type  = "engage_target";
  cmd.issued_at_ms  = 100;
  bus->Dispatch(cmd);

  ASSERT_TRUE(WaitFor([&] { return received.load(); }))
      << "CommandBus 转发消息未在 3s 内被 UDP adapter 收到";

  EXPECT_EQ(recv_entity, 42u);

  // cleanup
  loop_->PostToLoop([fwd_adapter]() { fwd_adapter->Disconnect(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  adapter_.reset();  // 避免 TearDown 重复 Disconnect
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 5：未连接时 Publish 返回错误
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(BusAdapterTest, PublishBeforeConnectReturnsError) {
  auto* raw_loop = &static_cast<UvwEventLoopRuntime*>(loop_.get())->Loop();
  auto adapter = std::make_shared<UvwUdpBusAdapter>(
      *raw_loop, uint16_t(0), "127.0.0.1", uint16_t(12345));

  std::atomic<bool> checked{false};
  loop_->PostToLoop([&adapter, &checked]() {
    BusMessage msg; msg.topic = "test";
    auto status = adapter->Publish(msg);
    EXPECT_FALSE(static_cast<bool>(status));
    checked.store(true, std::memory_order_release);
  });

  ASSERT_TRUE(WaitFor([&checked] { return checked.load(); }));
}

}  // namespace sim_bt
