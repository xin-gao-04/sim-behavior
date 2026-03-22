# sim-behavior

高性能多实体行为树运行时后端。

基于 **BehaviorTree.CPP v4 + oneTBB + uvw** 三层执行模型，提供：
- **Lock-Free Tick** — TBB 回调不阻塞主线程 Tick
- **Active Set** — kSkipIdle 模式仅遍历活跃实体，空闲树零开销
- **Wakeup 去重** — 同帧多次唤醒自动合并，消除冗余 Tick
- **DrainAll 批量化** — N+1 次锁降至 2 次锁，回调在锁外执行

> **离线构建**：所有依赖 zip vendor，克隆即可 `cmake + make`，无需网络。

---

## 架构概述

```
┌──────────────────────────────────────────────────────────┐
│  Layer 1 — Simulation Host                               │  进程入口 / 配置 / 装配
├──────────────────────────────────────────────────────────┤
│  Layer 2 — Behavior Runtime   (BehaviorTree.CPP)         │  树工厂 / Tick 调度 / Active Set
├──────────────────────────────────────────────────────────┤
│  Layer 3 — Async Orchestration (uvw / libuv)             │  事件循环 / timer / 跨线程唤醒
├──────────────────────────────────────────────────────────┤
│  Layer 4 — Compute Execution  (oneTBB task_arena)        │  high/normal/low 3 arena / 结果邮箱
├──────────────────────────────────────────────────────────┤
│  Layer 5 — Domain State                                  │  实体 / 编队 / 世界状态
├──────────────────────────────────────────────────────────┤
│  Layer 6 — Integration Adapters                          │  CommandBus / 总线 / 算法适配
└──────────────────────────────────────────────────────────┘
```

详细架构说明（含 Mermaid 图）见 [docs/design/architecture.md](docs/design/architecture.md)。

---

## 快速开始

```bash
# 1. 克隆 & 初始化子模块
git clone https://github.com/xin-gao-04/sim-behavior.git
cd sim-behavior
git submodule update --init --recursive

# 2. 构建（依赖从 zip 自动解压，全程离线）
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)

# 3. 运行测试（61 项，全绿）
./build/tests/sim_behavior_tests
```

> **依赖解析策略**（`cmake/Dependencies.cmake`）：
> 1. `third_party/<dep>/CMakeLists.txt` → 直接 `add_subdirectory`
> 2. `third_party/<dep>.zip` → 自动解压后 `add_subdirectory`
> 3. FetchContent → 在线下载（仅当前两者不存在时）

<details>
<summary>更新 vendor zip（联网机器）</summary>

```bash
bash scripts/vendor-deps.sh
git add third_party/*.zip
git commit -m "vendor: update third-party zips"
```

</details>

---

## 依赖版本

| 依赖 | 版本 | CMake 目标 | 引入方式 |
|------|------|-----------|---------|
| [corekit](https://github.com/xin-gao-04/corekit) | main | `corekit` | git submodule |
| [oneTBB](https://github.com/oneapi-src/oneTBB) | v2022.0.0 | `TBB::tbb` | zip vendor |
| [libuv](https://github.com/libuv/libuv) | v1.48.0 | `uv::uv` | zip vendor |
| [uvw](https://github.com/skypjack/uvw) | v3.4.0_libuv_v1.48 | `uvw::uvw` | zip vendor (header-only) |
| [BehaviorTree.CPP](https://github.com/BehaviorTree/BehaviorTree.CPP) | 4.9.0 | `BT::behaviortree_cpp` | zip vendor |
| [SQLite3](https://www.sqlite.org/) | 3.47.2 | `SQLite::SQLite3` | amalgamation vendor |
| [GoogleTest](https://github.com/google/googletest) | v1.16.0 | `GTest::gtest` | zip vendor |

**CMake ≥ 3.24 | C++17**

---

## CMake 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `SIMBEHAVIOR_BUILD_TESTS` | ON | 构建 GoogleTest 单元测试 |
| `SIMBEHAVIOR_BUILD_SIM_HOST` | ON | 构建 sim_host 可执行文件 |
| `SIMBEHAVIOR_ENABLE_ASAN` | OFF | AddressSanitizer（仅 GCC/Clang） |

---

## 目录结构

```
sim-behavior/
├── CMakeLists.txt
├── cmake/
│   ├── CompilerFlags.cmake          跨平台编译标志（MSVC / GCC / Clang）
│   └── Dependencies.cmake           三级依赖引入（目录 → zip → FetchContent）
├── include/sim_bt/                  公开接口（纯虚类，无实现细节）
│   ├── common/                      types.hpp, result.hpp
│   ├── runtime/bt_runtime/          IBtRuntime（TickStats / TickPolicy / SqliteLogger）
│   ├── runtime/async_runtime/       IEventLoopRuntime, IWakeupBridge, IBusAdapter
│   ├── runtime/compute_runtime/     IJobExecutor, IJobHandle, IResultMailbox
│   ├── domain/                      IEntityContext, IGroupContext, IWorldSnapshot
│   ├── adapters/                    ICommandBus
│   └── bt_nodes/                    AsyncActionBase, IAsyncActionContext
├── src/                             具体实现（不对外暴露）
│   ├── runtime/compute_runtime/     TbbJobExecutor（SystemPool 内存池分配）
│   ├── runtime/async_runtime/       UvwEventLoopRuntime, UvwWakeupBridge
│   ├── runtime/bt_runtime/          BtRuntimeImpl（Lock-Free Tick / Active Set）
│   ├── domain/                      EntityContextImpl, GroupContextImpl
│   ├── adapters/                    InProcessCommandBus, UvwUdpBusAdapter
│   ├── bt_nodes/                    AsyncActionBase 实现
│   └── sim_host/                    SimHostApp + main.cpp
├── tests/                           GoogleTest 测试套件（61 项）
│   ├── test_cancellation_token.cpp
│   ├── test_result_mailbox.cpp
│   ├── test_entity_context.cpp
│   ├── test_async_action_base.cpp
│   ├── test_cross_library_integration.cpp   TBB↔Mailbox↔uvw 跨库边界
│   ├── test_multi_entity.cpp                多实体隔离 + 编队
│   ├── test_group_context.cpp               GroupContext 共享状态
│   ├── test_bus_adapter.cpp                 UDP BusAdapter
│   ├── test_phase4_perf.cpp                 arena 隔离 + TickStats + kSkipIdle
│   ├── test_phase5_perf.cpp                 Lock-Free / ActiveSet / DrainAll 验证
│   └── test_e2e_async_action.cpp            端到端异步 Action 集成测试
├── docs/
│   ├── design/architecture.md       六层架构详细设计（含 Mermaid 图）
│   └── html/                        生成文档
├── scripts/
│   └── vendor-deps.sh               联网机器一键下载所有 zip 依赖
└── third_party/
    ├── corekit/                     git submodule
    ├── sqlite3/                     SQLite 3.47.2 amalgamation
    ├── oneTBB.zip
    ├── libuv.zip
    ├── uvw.zip
    ├── BehaviorTree.CPP.zip
    └── googletest.zip
```

---

## 开发节点指南

### 同步条件节点

```cpp
class HasTargetCondition : public BT::ConditionNode {
  BT::NodeStatus tick() override {
    return ctx_->CurrentTarget() != kInvalidEntityId
        ? BT::NodeStatus::SUCCESS : BT::NodeStatus::FAILURE;
  }
};
```

### CPU 密集型异步动作节点

```cpp
class ComputeAction : public sim_bt::AsyncActionBase {
 public:
  sim_bt::NodeStatus OnStart() override {
    job_id_ = Ctx().SubmitCpuJob(
      sim_bt::JobPriority::kNormal,
      [](sim_bt::CancellationTokenPtr tok, sim_bt::JobResult& out) {
        if (tok->IsCancelled()) return;
        out.succeeded = true;  // CPU 密集计算（路径规划、评分等）
      }
    )->JobId();
    Ctx().StartTimeout(std::chrono::milliseconds(80));
    return sim_bt::NodeStatus::kRunning;
  }

  sim_bt::NodeStatus OnRunning() override {
    if (Ctx().IsTimedOut()) return sim_bt::NodeStatus::kFailure;
    auto r = Ctx().PeekResult(job_id_);
    if (!r) return sim_bt::NodeStatus::kRunning;
    Ctx().ConsumeResult(job_id_);
    Ctx().CancelTimeout();
    return r->succeeded ? sim_bt::NodeStatus::kSuccess
                        : sim_bt::NodeStatus::kFailure;
  }

  void OnHalted() override {
    Ctx().CancelJob(job_id_);
    Ctx().CancelTimeout();
  }

 private:
  uint64_t job_id_ = 0;
};
```

### 内存池统计

```cpp
auto stats = app.MemoryPoolStats();
printf("alloc=%llu, in_use=%lluB, peak=%lluB, slab_hits=%llu\n",
       stats.alloc_count, stats.bytes_in_use, stats.bytes_peak, stats.slab_hits);
```

---

## 路线图

| 阶段 | 内容 | 状态 |
|------|------|------|
| Phase 1 | 最小闭环：单实体、TBB 任务 → uvw wakeup → re-tick | ✅ 完成 |
| Phase 2 | 多实体、GroupContext、WorldSnapshot 完整集成 | ✅ 完成 |
| Phase 3 | uvw UDP BusAdapter、仿真宿主总线接入 | ✅ 完成 |
| Phase 4 | 性能治理：arena 隔离、kSkipIdle、TickStats、TraceLogger、内存池 | ✅ 完成 |
| Phase 5 | 性能优化：Lock-Free Tick、Active Set、Wakeup 去重、DrainAll 批量化 | ✅ 完成 |

---

## 编译验证

| 平台 | 编译器 | 状态 |
|------|--------|------|
| macOS 14 (ARM64) | AppleClang 17 | ✅ 61/61 tests pass |
| Ubuntu 22.04 | GCC 13 | 🏗 待验证 |
| Windows Server 2022 | MSVC 2022 | 🏗 待验证 |
