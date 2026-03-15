# sim-behavior

陆军仿真系统行为树后端模块。

基于 **BehaviorTree.CPP v4 + oneTBB + uvw** 三层执行模型，
实现高性能、可维护的多实体行为树运行时。

> **所有依赖均从源码编译，不依赖系统安装包。**  
> 适合跨平台部署和内网离线环境。

---

## 架构概述

```
┌──────────────────────────────────────────────────────────┐
│  Layer 1 — Simulation Host                               │  进程入口 / 配置 / 装配
├──────────────────────────────────────────────────────────┤
│  Layer 2 — Behavior Runtime   (BehaviorTree.CPP)         │  树工厂 / Tick调度 / wakeup队列
├──────────────────────────────────────────────────────────┤
│  Layer 3 — Async Orchestration (uvw / libuv)             │  事件循环 / timer / 跨线程唤醒
├──────────────────────────────────────────────────────────┤
│  Layer 4 — Compute Execution  (oneTBB task_arena)        │  高/中/低 3个arena / 结果邮箱
├──────────────────────────────────────────────────────────┤
│  Layer 5 — Domain State                                  │  实体/编队/世界状态
├──────────────────────────────────────────────────────────┤
│  Layer 6 — Integration Adapters                          │  CommandBus / 总线 / 算法适配
└──────────────────────────────────────────────────────────┘
```

详细架构说明见 [docs/design/architecture.md](docs/design/architecture.md)。

---

## 依赖一览

| 依赖 | 版本 | 引入方式 | CMake 目标 |
|------|------|----------|-----------|
| [corekit](third_party/corekit) | main | **git submodule（必须）** | `corekit` |
| [oneTBB](https://github.com/oneapi-src/oneTBB) | v2022.0.0 | submodule / FetchContent | `TBB::tbb` |
| [libuv](https://github.com/libuv/libuv) | v1.48.0 | submodule / FetchContent | `uv::uv` |
| [uvw](https://github.com/skypjack/uvw) | v3.4.0 | submodule / FetchContent | `uvw::uvw` |
| [BehaviorTree.CPP](https://github.com/BehaviorTree/BehaviorTree.CPP) | 4.6.2 | submodule / FetchContent | `BT::behaviortree_cpp` |
| [GoogleTest](https://github.com/google/googletest) | v1.14.0 | submodule / FetchContent | `GTest::gtest` |

CMake 最低版本：3.24 | C++ 标准：17

---

## 快速开始（在线环境）

```bash
# 1. 克隆仓库并初始化 corekit 子模块
git clone <repo-url> sim-behavior && cd sim-behavior
git submodule update --init --recursive   # 初始化 corekit

# 2. 构建（其余依赖首次构建时自动从 GitHub 下载）
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)

# 3. 运行测试
cd build && ctest --output-on-failure
```

---

## 内网离线部署

内网部署无法访问 GitHub，需要提前准备好所有依赖的源码镜像。

### 方式 A：git submodule（推荐）

将各依赖镜像到内网 Git 服务器后，把它们添加为子模块：

```bash
# 在 sim-behavior 根目录执行
git submodule add <内网镜像>/oneTBB.git         third_party/oneTBB
git submodule add <内网镜像>/libuv.git           third_party/libuv
git submodule add <内网镜像>/uvw.git             third_party/uvw
git submodule add <内网镜像>/BehaviorTree.CPP.git third_party/BehaviorTree.CPP
git submodule add <内网镜像>/googletest.git       third_party/googletest

# 之后所有人 clone 后只需
git submodule update --init --recursive
```

CMake 会自动检测 `third_party/<dep>/CMakeLists.txt` 并优先使用，**不会触发网络下载**。

### 方式 B：本地路径覆盖

若已有本地解压的源码目录，无需修改 .gitmodules，直接传参：

```bash
cmake -B build \
  -DSIMBEHAVIOR_TBB_SOURCE_DIR=/opt/deps/oneTBB    \
  -DSIMBEHAVIOR_LIBUV_SOURCE_DIR=/opt/deps/libuv   \
  -DSIMBEHAVIOR_UVW_SOURCE_DIR=/opt/deps/uvw       \
  -DSIMBEHAVIOR_BTCPP_SOURCE_DIR=/opt/deps/BT.CPP  \
  -DSIMBEHAVIOR_GTEST_SOURCE_DIR=/opt/deps/gtest
cmake --build build -j$(nproc)
```

---

## CMake 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `SIMBEHAVIOR_BUILD_TESTS` | ON | 构建 GoogleTest 单元测试 |
| `SIMBEHAVIOR_BUILD_TOOLS` | ON | 构建开发工具 |
| `SIMBEHAVIOR_BUILD_SIM_HOST` | ON | 构建 sim_host 可执行文件 |
| `SIMBEHAVIOR_ENABLE_ASAN` | OFF | AddressSanitizer（GCC/Clang） |
| `SIMBEHAVIOR_TBB_SOURCE_DIR` | "" | oneTBB 本地源码路径 |
| `SIMBEHAVIOR_LIBUV_SOURCE_DIR` | "" | libuv 本地源码路径 |
| `SIMBEHAVIOR_UVW_SOURCE_DIR` | "" | uvw 本地源码路径 |
| `SIMBEHAVIOR_BTCPP_SOURCE_DIR` | "" | BehaviorTree.CPP 本地源码路径 |
| `SIMBEHAVIOR_GTEST_SOURCE_DIR` | "" | googletest 本地源码路径 |

---

## 目录结构

```
sim-behavior/
├── CMakeLists.txt
├── cmake/
│   ├── CompilerFlags.cmake       跨平台编译标志
│   └── Dependencies.cmake        依赖引入（submodule→本地路径→FetchContent）
├── include/sim_bt/               公开接口（纯虚类）
│   ├── common/                   types.hpp, result.hpp
│   ├── runtime/bt_runtime/       IBtRuntime, ITreeInstance
│   ├── runtime/async_runtime/    IEventLoopRuntime, IWakeupBridge, IBusAdapter
│   ├── runtime/compute_runtime/  IJobExecutor, IJobHandle, IResultMailbox, ICancellationToken
│   ├── domain/                   IEntityContext, IGroupContext, IWorldSnapshot
│   ├── adapters/                 ICommandBus
│   └── bt_nodes/                 AsyncActionBase, IAsyncActionContext
├── src/                          具体实现
│   ├── runtime/compute_runtime/  TbbJobExecutor, DefaultResultMailbox
│   ├── runtime/async_runtime/    UvwEventLoopRuntime, UvwWakeupBridge
│   ├── runtime/bt_runtime/       BtRuntimeImpl, AsyncActionContextImpl
│   ├── domain/                   实体/编队/快照实现
│   ├── adapters/                 InProcessCommandBus
│   ├── bt_nodes/                 AsyncActionBase 实现
│   └── sim_host/                 SimHostApp + main.cpp
├── tests/                        GoogleTest 单元测试
├── tools/                        开发工具（后续迭代）
├── docs/design/
│   ├── architecture.md           六层架构详细设计
│   └── behaviorTree+onetbb+uvw.md 原始设计文档
└── third_party/
    ├── corekit/                  git submodule（必须）
    ├── oneTBB/                   git submodule（可选，否则 FetchContent）
    ├── libuv/                    git submodule（可选，否则 FetchContent）
    ├── uvw/                      git submodule（可选，否则 FetchContent）
    ├── BehaviorTree.CPP/         git submodule（可选，否则 FetchContent）
    └── googletest/               git submodule（可选，否则 FetchContent）
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
class PathPlanAction : public sim_bt::AsyncActionBase {
  sim_bt::NodeStatus OnStart() override {
    job_id_ = Ctx().SubmitCpuJob(JobPriority::kNormal,
      [](CancellationTokenPtr token, JobResult& out) {
        // TBB worker 线程：路径规划算法
        if (token->IsCancelled()) return;
        out.succeeded = true;
        out.payload = PathResult{...};
      })->JobId();
    Ctx().StartTimeout(std::chrono::milliseconds(80));
    return NodeStatus::kRunning;
  }
  sim_bt::NodeStatus OnRunning() override {
    auto r = Ctx().PeekResult(job_id_);
    if (!r) return NodeStatus::kRunning;
    Ctx().ConsumeResult(job_id_);
    return r->succeeded ? NodeStatus::kSuccess : NodeStatus::kFailure;
  }
  void OnHalted() override {
    Ctx().CancelJob(job_id_);
    Ctx().CancelTimeout();
  }
  uint64_t job_id_ = 0;
};
```

---

## 路线图

| 阶段 | 内容 | 状态 |
|------|------|------|
| Phase 1 | 最小闭环：单实体、TBB任务→uvw wakeup→re-tick | 🏗 进行中 |
| Phase 2 | 多实体、WorldSnapshot、端到端结果消费 | ⏳ |
| Phase 3 | uvw BusAdapter、仿真宿主总线接入 | ⏳ |
| Phase 4 | 性能治理：多arena优先级、批量tick | ⏳ |
