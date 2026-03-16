#include "sim_host_app.hpp"

#include <chrono>
#include <thread>

// 具体实现引用（内部 include，不暴露到 public interface）
#include "../runtime/compute_runtime/tbb_job_executor.hpp"
#include "../runtime/async_runtime/uvw_event_loop_runtime.hpp"
#include "sim_bt/common/sim_bt_log.hpp"

namespace sim_bt {

// 工厂函数声明（defined in their respective .cpp files）
std::shared_ptr<IBtRuntime>  CreateBtRuntime();
std::shared_ptr<ICommandBus> CreateInProcessCommandBus();

SimHostApp::SimHostApp() {
  // 尽早初始化进程级 logger，使后续所有 SIMBT_LOG_* 调用立即生效
  InitSimBtLog("sim_host");
}

SimHostApp::~SimHostApp() {
  RequestStop();
  ShutdownSimBtLog();
}

SimStatus SimHostApp::Initialize() {
  SIMBT_LOG_INFO("SimHostApp: initializing");
  // ── 创建各运行时 ──────────────────────────────────────────────────────────

  // Compute Runtime
  ArenaConfig arena_cfg;
  arena_cfg.high_concurrency   = 2;
  arena_cfg.normal_concurrency = 4;
  arena_cfg.low_concurrency    = 2;
  auto executor = std::make_shared<TbbJobExecutor>(arena_cfg);
  job_executor_ = executor;

  // Async Runtime (uvw)
  auto event_loop = std::make_shared<UvwEventLoopRuntime>();
  event_loop_ = event_loop;

  // BT Runtime（需在 wakeup callback 设置前创建，否则 weak_ptr 为空）
  bt_runtime_ = CreateBtRuntime();
  auto status = bt_runtime_->Initialize();
  if (!status) return status;

  // 将 TBB 结果写入 mailbox 后触发完整 wakeup 链路：
  //   TBB worker → mailbox.Post() → notify_cb
  //   → PostToLoop（投入 uvw loop 线程）
  //   → DrainAll（incoming_ → ready_ 并逐条回调）
  //   → RequestWakeup(owner_entity)
  //   → 下一帧 TickAll Phase 1 优先 tick 该实体
  auto mailbox_ptr   = executor->Mailbox_shared();
  auto bt_runtime_wk = std::weak_ptr<IBtRuntime>(bt_runtime_);
  executor->SetWakeupCallback(
      [event_loop_ptr = event_loop.get(),
       mailbox_ptr,
       bt_runtime_wk]() {
        event_loop_ptr->PostToLoop(
            [mailbox_ptr, bt_runtime_wk]() {
              // 在 uvw loop 线程：批量 drain，逐条 RequestWakeup
              mailbox_ptr->DrainAll([bt_runtime_wk](JobResult r) {
                if (auto bt = bt_runtime_wk.lock()) {
                  if (r.owner_entity != kInvalidEntityId) {
                    bt->RequestWakeup(r.owner_entity);
                  }
                }
              });
            });
      });

  // Command Bus
  command_bus_ = CreateInProcessCommandBus();

  // World Snapshot Provider（简单实现）
  // snapshot_provider_ = std::make_shared<SimpleWorldSnapshotProvider>();

  // 启动 EventLoop
  status = event_loop_->Start();
  if (!status) return status;

  SIMBT_LOG_INFO("SimHostApp: initialization complete");
  return SimStatus::Ok();
}

void SimHostApp::Run() {
  TickLoop();
}

void SimHostApp::RequestStop() {
  SIMBT_LOG_INFO_S("SimHostApp: stop requested at sim_time=" << current_sim_time_ << "ms");
  stop_requested_ = true;
  if (event_loop_) event_loop_->Stop();
  if (job_executor_) job_executor_->Shutdown();
  if (bt_runtime_) bt_runtime_->Shutdown();
}

void SimHostApp::TickLoop() {
  // 固定帧率：20 Hz（50ms/frame），可配置
  constexpr auto kFrameInterval = std::chrono::milliseconds(50);

  while (!stop_requested_) {
    auto frame_start = std::chrono::steady_clock::now();

    current_sim_time_ += 50;  // 每帧推进 50ms

    // 1. 刷新世界快照
    // snapshot_provider_->Refresh(current_sim_time_);

    // 2. Tick 所有活跃实体的行为树
    bt_runtime_->TickAll(current_sim_time_);

    // 3. 等到下一帧
    auto elapsed = std::chrono::steady_clock::now() - frame_start;
    auto sleep_time = kFrameInterval - elapsed;
    if (sleep_time > std::chrono::milliseconds(0)) {
      std::this_thread::sleep_for(sleep_time);
    }
  }
}

}  // namespace sim_bt
