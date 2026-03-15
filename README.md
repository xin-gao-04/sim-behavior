# sim-behavior

通用行为树后端模块。

基于 **BehaviorTree.CPP v4 + oneTBB + uvw** 三层执行模型，
实现高性能、可维护的多实体行为树运行时。

> **所有依赖均从源码编译，zip 离线 vendor，不依赖系统安装包。**
> 克隆仓库后无需网络，直接 `cmake + make` 即可在内网机器上完整构建。

---

## 架构概述

```
┌──────────────────────────────────────────────────────────┐
│  Layer 1 — Simulation Host                               │  进程入口 / 配置 / 装配
├──────────────────────────────────────────────────────────┤
│  Layer 2 — Behavior Runtime   (BehaviorTree.CPP)         │  树工厂 / Tick 调度 / wakeup 队列
├──────────────────────────────────────────────────────────┤
│  Layer 3 — Async Orchestration (uvw / libuv)             │  事件循环 / timer / 跨线程唤醒
├──────────────────────────────────────────────────────────┤
│  Layer 4 — Compute Execution  (oneTBB task_arena)        │  high/normal/low 3 个 arena / 结果邮箱
├──────────────────────────────────────────────────────────┤
│  Layer 5 — Domain State                                  │  实体 / 编队 / 世界状态
├──────────────────────────────────────────────────────────┤
│  Layer 6 — Integration Adapters                          │  CommandBus / 总线 / 算法适配
└──────────────────────────────────────────────────────────┘
```

详细架构说明（含 Mermaid 图）见 [docs/design/architecture.md](docs/design/architecture.md)。

---

## 依赖版本

| 依赖 | 版本 | CMake 目标 | 引入方式 |
|------|------|-----------|---------|
| [corekit](https://github.com/xin-gao-04/corekit) | main | `corekit` | **git submodule（必须）** |
| [oneTBB](https://github.com/oneapi-src/oneTBB) | v2022.0.0 | `TBB::tbb` | zip vendor → `add_subdirectory` |
| [libuv](https://github.com/libuv/libuv) | v1.48.0 | `uv::uv` | zip vendor → `add_subdirectory` |
| [uvw](https://github.com/skypjack/uvw) | v3.4.0_libuv_v1.48 | `uvw::uvw` | zip vendor → header-only |
| [BehaviorTree.CPP](https://github.com/BehaviorTree/BehaviorTree.CPP) | 4.9.0 | `BT::behaviortree_cpp` | zip vendor → `add_subdirectory` |
| [GoogleTest](https://github.com/google/googletest) | v1.16.0 | `GTest::gtest` | zip vendor → `add_subdirectory` |

**CMake 最低版本：3.24 | C++ 标准：C++17**

---

## 快速开始

### 方式一：内网 / 离线（推荐，零依赖网络）

仓库已包含所有依赖的 `.zip` 源码包（`third_party/*.zip`），cmake 配置时自动解压，无需任何网络访问。

```bash
# 1. 克隆仓库并初始化 corekit 子模块
git clone https://github.com/xin-gao-04/sim-behavior.git
cd sim-behavior
git submodule update --init --recursive   # 拉取 corekit

# 2. 构建（依赖从 zip 自动解压，全程不需网络）
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)

# 3. 运行测试
./build/tests/sim_behavior_tests
```

### 方式二：在线环境（zip 不存在时自动 FetchContent）

```bash
git clone https://github.com/xin-gao-04/sim-behavior.git
cd sim-behavior
git submodule update --init --recursive

cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
./build/tests/sim_behavior_tests
```

> cmake/Dependencies.cmake 依次尝试：
> 1. `third_party/<dep>/CMakeLists.txt` → 直接 `add_subdirectory`
> 2. `third_party/<dep>.zip` → 自动解压后 `add_subdirectory`
> 3. FetchContent → 在线下载（需要网络）

### 重新下载 zip 包（在联网机器上更新 vendor）

```bash
bash scripts/vendor-deps.sh
git add third_party/*.zip
git commit -m "vendor: update third-party zips"
```

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
│   ├── CompilerFlags.cmake       跨平台编译标志（MSVC / GCC / Clang）
│   └── Dependencies.cmake        三级依赖引入（目录 → zip → FetchContent）
├── include/sim_bt/               公开接口（纯虚类，无实现细节）
│   ├── common/                   types.hpp, result.hpp
│   ├── runtime/bt_runtime/       IBtRuntime, ITreeInstance
│   ├── runtime/async_runtime/    IEventLoopRuntime, IWakeupBridge, IBusAdapter
│   ├── runtime/compute_runtime/  IJobExecutor, IJobHandle, IResultMailbox, ICancellationToken
│   ├── domain/                   IEntityContext, IGroupContext, IWorldSnapshot
│   ├── adapters/                 ICommandBus
│   └── bt_nodes/                 AsyncActionBase, IAsyncActionContext
├── src/                          具体实现（不对外暴露）
│   ├── runtime/compute_runtime/  TbbJobExecutor, DefaultResultMailbox
│   ├── runtime/async_runtime/    UvwEventLoopRuntime, UvwWakeupBridge
│   ├── runtime/bt_runtime/       BtRuntimeImpl, AsyncActionContextImpl
│   ├── domain/                   EntityContextImpl, GroupContextImpl, WorldSnapshotImpl
│   ├── adapters/                 InProcessCommandBus
│   ├── bt_nodes/                 AsyncActionBase 实现
│   └── sim_host/                 SimHostApp + main.cpp（进程入口）
├── tests/                        GoogleTest 测试套件
│   ├── test_cancellation_token.cpp          # ICancellationToken 单元测试
│   ├── test_result_mailbox.cpp              # IResultMailbox 单元测试
│   ├── test_entity_context.cpp              # IEntityContext 单元测试
│   ├── test_async_action_base.cpp           # AsyncActionBase 单元测试
│   └── test_cross_library_integration.cpp  # 跨库边界集成测试（TBB↔Mailbox↔uvw）
├── scripts/
│   └── vendor-deps.sh            联网机器一键下载所有 zip 依赖
├── docs/design/
│   ├── architecture.md           六层架构详细设计（含 Mermaid 图）
│   └── behaviorTree+onetbb+uvw.md 原始设计文档
└── third_party/
    ├── corekit/                  git submodule（必须克隆）
    ├── oneTBB.zip                ← 已入库，cmake 自动解压
    ├── libuv.zip
    ├── uvw.zip
    ├── BehaviorTree.CPP.zip
    └── googletest.zip
```

---

## 开发节点指南（快速参考）

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
        // TBB worker 线程：CPU 密集计算（路径规划、评分等）
        out.succeeded = true;
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

---

## 路线图

| 阶段 | 内容 | 状态 |
|------|------|------|
| Phase 1 | 最小闭环：单实体、TBB 任务 → uvw wakeup → re-tick | 🏗 进行中 |
| Phase 2 | 多实体、WorldSnapshot 完整集成、端到端结果消费 | ⏳ |
| Phase 3 | uvw TCP/UDP BusAdapter、仿真宿主总线接入 | ⏳ |
| Phase 4 | 性能治理：多 arena 优先级验证、批量 tick、TraceLogger | ⏳ |

---

## 编译验证（CI 矩阵目标）

| 平台 | 编译器 | 状态 |
|------|--------|------|
| macOS 14 (ARM64) | AppleClang 17 | ✅ 21/21 tests pass |
| Ubuntu 22.04 | GCC 13 | 🏗 待验证 |
| Windows Server 2022 | MSVC 2022 | 🏗 待验证 |
