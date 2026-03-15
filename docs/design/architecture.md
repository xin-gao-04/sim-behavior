# sim-behavior 架构设计

## 1. 设计目标

- **行为树语义与执行分离**：BehaviorTree.CPP 负责"该做什么"，oneTBB 负责"谁来算"，uvw 负责"什么时候回来"
- **线程边界清晰**：三类执行域不允许跨域直接操作共享状态
- **BT 节点可单元测试**：节点只依赖接口 Facade（`IAsyncActionContext`），不直接持有 uvw/TBB 原始对象
- **跨平台 & 内网部署**：所有依赖从源码编译，zip 离线 vendor，不依赖系统安装包

---

## 2. 六层架构总览

```mermaid
graph TB
    L1["Layer 1 — Simulation Host<br/>SimHostApp<br/>进程生命周期 / 配置加载 / 模块装配 / 时钟驱动"]
    L2["Layer 2 — Behavior Runtime<br/>BtRuntimeImpl<br/>BT 工厂 / 节点注册 / 树实例管理 / TickScheduler / wakeup队列"]
    L3["Layer 3 — Async Orchestration<br/>UvwEventLoopRuntime<br/>事件循环 / 定时器 / 跨线程 PostToLoop / WakeupBridge"]
    L4["Layer 4 — Compute Execution<br/>TbbJobExecutor<br/>high/normal/low 三个 task_arena / DefaultResultMailbox"]
    L5["Layer 5 — Domain State<br/>EntityContext / GroupContext / WorldSnapshot<br/>实体私有态 / 编队共享态 / 全局只读态势"]
    L6["Layer 6 — Integration Adapters<br/>CommandBus / IBusAdapter<br/>命令出站 / 仿真总线接入 / 传感器适配"]

    L1 -->|"装配 & 驱动"| L2
    L2 -->|"StartTimeout / EmitWakeup"| L3
    L2 -->|"SubmitCpuJob"| L4
    L3 -->|"RequestWakeup"| L2
    L4 -->|"Post(result) → notify_cb"| L3
    L2 <-->|"读写"| L5
    L2 -->|"Dispatch(ActionCommand)"| L6
```

---

## 3. 三类线程域

```mermaid
graph LR
    subgraph BT_TICK["BT Tick Domain（主循环线程）"]
        TA["BtRuntime::TickAll()"]
        TB["tree->Tick()"]
        TC["BT 黑板读写"]
    end

    subgraph UVW_LOOP["uvw Event Domain（独立线程）"]
        UA["UvwEventLoopRuntime::loop_thread_"]
        UB["timer 回调"]
        UC["PostToLoop 回调"]
        UD["WakeupBridge::DrainAll()"]
    end

    subgraph TBB_COMPUTE["TBB Compute Domain（worker 线程池）"]
        CA["arena_high_ (×2)"]
        CB["arena_normal_ (×4)"]
        CC["arena_low_ (×2)"]
    end

    CA -->|"Post(result)"| MB["DefaultResultMailbox"]
    CB -->|"Post(result)"| MB
    CC -->|"Post(result)"| MB
    MB -->|"notify_cb → async_handle.send()"| UD
    UD -->|"RequestWakeup(entity_id)"| TA
    TA --> TB --> TC
```

**跨域通信规则（强制）**：

| 来源域 | 目标域 | 允许的操作 |
|--------|--------|-----------|
| TBB Compute | Mailbox | `Post(result)` — 唯一出口 |
| Mailbox → uvw | uvw loop | `async_handle.send()` 唤醒 |
| uvw loop | BT Tick | `RequestWakeup(entity_id)` — 加入 wakeup 队列 |
| BT Tick | TBB | `SubmitCpuJob()` — 提交任务 |

**绝对禁止**：TBB worker 线程直接修改 BT 黑板、调用 `EntityContext::set*()`、调用 `BtRuntime::TickEntity()`。

---

## 4. 核心接口关系

```mermaid
classDiagram
    class IAsyncActionContext {
        <<interface>>
        +SubmitCpuJob(priority, task) JobHandlePtr
        +CancelJob(job_id)
        +PeekResult(job_id) optional~JobResult~
        +ConsumeResult(job_id)
        +StartTimeout(ms)
        +CancelTimeout()
        +IsTimedOut() bool
        +EmitWakeup()
        +OwnerEntity() EntityId
        +CurrentSimTime() SimTimeMs
    }

    class IJobExecutor {
        <<interface>>
        +Submit(priority, task) JobHandlePtr
        +Shutdown()
    }

    class IResultMailbox {
        <<interface>>
        +Post(job_id, result)
        +SetNotifyCallback(cb)
        +DrainAll()
        +Peek(job_id) optional~JobResult~
        +Consume(job_id)
        +Discard(job_id)
    }

    class IEventLoopRuntime {
        <<interface>>
        +Start()
        +Stop()
        +PostToLoop(fn)
        +StartOneShotTimer(ms, cb) TimerHandlePtr
    }

    class IBtRuntime {
        <<interface>>
        +RegisterTree(xml, nodes)
        +CreateTree(entity_id, tree_name, bb) TreeInstancePtr
        +TickAll(sim_time)
        +RequestWakeup(entity_id)
    }

    class AsyncActionBase {
        <<abstract>>
        #OnStart() NodeStatus
        #OnRunning() NodeStatus
        #OnHalted()
        +Ctx() IAsyncActionContext&
    }

    IAsyncActionContext --> IJobExecutor : SubmitCpuJob
    IAsyncActionContext --> IResultMailbox : PeekResult/ConsumeResult
    IAsyncActionContext --> IEventLoopRuntime : StartTimeout
    IAsyncActionContext --> IBtRuntime : EmitWakeup to RequestWakeup
    AsyncActionBase --> IAsyncActionContext : 唯一依赖
```

---

## 5. 异步动作生命周期

```mermaid
sequenceDiagram
    participant HOST as SimHostApp
    participant BT as BtRuntime::TickAll
    participant NODE as AsyncActionBase
    participant CTX as AsyncActionContextImpl
    participant TBB as TbbJobExecutor
    participant MB as DefaultResultMailbox
    participant UVW as UvwEventLoopRuntime

    HOST->>BT: TickAll(sim_time)
    BT->>BT: drain wakeup_queue_
    BT->>NODE: tree->Tick() → onStart()
    NODE->>CTX: SubmitCpuJob(kNormal, task)
    CTX->>TBB: arena_normal_.execute(tg.run(task))
    NODE->>CTX: StartTimeout(80ms)
    CTX->>UVW: PostToLoop(create timer)
    NODE-->>BT: NodeStatus::kRunning

    Note over TBB: worker 线程执行计算
    TBB->>MB: Post(job_id, result)
    MB->>UVW: notify_cb() → async_handle.send()

    UVW->>UVW: loop 回调: DrainAll()
    UVW->>BT: RequestWakeup(entity_id)

    HOST->>BT: 下一帧 TickAll()
    BT->>BT: 处理 wakeup_queue_ → 优先 tick 该实体
    BT->>NODE: tree->Tick() → onRunning()
    NODE->>CTX: PeekResult(job_id)
    CTX->>MB: Peek(job_id)
    MB-->>CTX: JobResult{succeeded=true}
    CTX-->>NODE: optional<JobResult>
    NODE->>CTX: ConsumeResult(job_id)
    NODE->>CTX: CancelTimeout()
    NODE-->>BT: NodeStatus::kSuccess
```

---

## 6. 节点分类与实现策略

| 节点类型 | 场景示例 | 实现基类 | 执行域 | 备注 |
|----------|----------|----------|--------|------|
| 同步条件节点 | `HasTarget`, `CheckResource` | `BT::ConditionNode` | BT Tick | 必须微秒级完成，不走 TBB/uvw |
| 同步瞬时动作 | `SetFlag`, `ResetTimer` | `BT::SyncActionNode` | BT Tick | 同上 |
| CPU 密集异步动作 | `ComputeAction`, `EvalAction` | `AsyncActionBase` | TBB Compute | `OnStart` 提交 arena 任务 |
| I/O 等待型动作 | `WaitBusEvent`, `WaitReply` | `AsyncActionBase` | uvw Event | `OnStart` 注册 loop handle |
| 混合型（超时保护） | `ComputeWithTimeout` | `AsyncActionBase` | TBB + uvw | TBB 算，uvw 超时 |

### AsyncActionBase 回调转发

```mermaid
stateDiagram-v2
    [*] --> IDLE
    IDLE --> RUNNING : OnStart - SubmitCpuJob + StartTimeout
    RUNNING --> SUCCESS : OnRunning - PeekResult succeeded
    RUNNING --> FAILURE : OnRunning - PeekResult failed
    RUNNING --> FAILURE : OnRunning - IsTimedOut, calls OnHalted
    RUNNING --> IDLE : OnHalted - CancelJob + CancelTimeout
    SUCCESS --> [*]
    FAILURE --> [*]
```

---

## 7. oneTBB Arena 配置

```mermaid
graph LR
    subgraph TBB["TbbJobExecutor"]
        AH["arena_high_\nconcurrency = 2\nJobPriority::kHigh"]
        AN["arena_normal_\nconcurrency = 4\nJobPriority::kNormal"]
        AL["arena_low_\nconcurrency = 2\nJobPriority::kLow"]
    end

    AH -->|"高优先级决策计算"| MB["DefaultResultMailbox"]
    AN -->|"路径规划 / 地形查询"| MB
    AL -->|"预取 / 后台统计"| MB
    MB -->|"notify_cb"| UVW["UvwEventLoopRuntime"]
```

| Arena | `JobPriority` | 并发数 | 典型任务 |
|-------|--------------|--------|----------|
| `arena_high_` | `kHigh` | 2 | 高优先级决策计算（时延敏感） |
| `arena_normal_` | `kNormal` | 4 | 路径规划、几何计算、地形查询 |
| `arena_low_` | `kLow` | 2 | 预取、后台统计、离线缓存 |

> **注意**：`reserved_for_masters` 不要配置过高，否则 `enqueue()` 的调度保证会失效（oneTBB 官方文档警告）。

---

## 8. 黑板分层设计

```mermaid
graph TB
    subgraph BB["黑板分层（生命周期从短到长）"]
        EB["EntityBlackboard\n(BT::Blackboard, per-entity)\n短生命周期\n当前目标 / 最近规划结果引用 / 局部状态标志"]
        GC["GroupContext\n(C++ 对象, per-group)\n中生命周期\n编队目标区 / 集结点 / 协调规则"]
        WS["WorldSnapshot\n(只读, per-frame)\n每帧刷新\n全局态势 / 目标池 / 观测汇总\nBT 只读取，不写入"]
    end

    EB --> GC --> WS
```

---

## 9. 标准帧数据流

```mermaid
flowchart TD
    A["SimHostApp::TickLoop()\n帧开始（20 Hz / 50ms）"]
    B["WorldSnapshotProvider::Refresh(sim_time)\n刷新只读全局态势"]
    C["BtRuntime::TickAll(sim_time)"]
    D["drain wakeup_queue_\n处理已就绪异步结果的实体"]
    E["逐实体 tree->Tick()"]
    F{"节点类型"}
    G["同步节点\n直接读 WorldSnapshot / EntityContext\n返回 SUCCESS / FAILURE"]
    H["异步节点 onStart()\nSubmitCpuJob() → TBB arena 入队\n返回 RUNNING"]
    I["异步节点 onRunning()\nPeekResult() → 若就绪返回 S/F\n否则返回 RUNNING"]
    J["TBB worker 计算完成\nmailbox.Post(result)"]
    K["notify_cb()\nuvw async_handle.send()"]
    L["uvw loop 回调\nDrainAll() → RequestWakeup(entity_id)\n加入 wakeup_queue_"]
    M["下一帧 TickAll() 优先处理该实体"]

    A --> B --> C --> D --> E --> F
    F -->|同步| G
    F -->|异步首次| H
    F -->|异步轮询| I
    H --> J --> K --> L --> M --> D
    I -->|"结果未就绪"| E
    I -->|"结果就绪"| G
```

---

## 10. 目录结构与模块职责

```
sim-behavior/
├── cmake/
│   ├── CompilerFlags.cmake       跨平台编译标志（MSVC / GCC / Clang）
│   └── Dependencies.cmake        三级依赖引入（目录 → zip → FetchContent）
│
├── include/sim_bt/               公开接口（纯虚类，无实现细节）
│   ├── common/
│   │   ├── types.hpp             EntityId, SimTimeMs, JobPriority 等基础类型
│   │   └── result.hpp            JobResult, NodeStatus 值语义结构体
│   ├── runtime/
│   │   ├── bt_runtime/           IBtRuntime, ITreeInstance
│   │   ├── async_runtime/        IEventLoopRuntime, IWakeupBridge, IBusAdapter
│   │   └── compute_runtime/      IJobExecutor, IJobHandle, IResultMailbox, ICancellationToken
│   ├── domain/
│   │   ├── entity/               IEntityContext
│   │   ├── group/                IGroupContext
│   │   └── world/                IWorldSnapshot
│   ├── adapters/                 ICommandBus
│   └── bt_nodes/
│       ├── i_async_action_context.hpp   BT 节点唯一运行时 Facade
│       └── async_action_base.hpp        所有异步节点的基类
│
├── src/                          具体实现（不对外暴露）
│   ├── runtime/
│   │   ├── compute_runtime/      TbbJobExecutor, TbbJobHandle, DefaultResultMailbox
│   │   ├── async_runtime/        UvwEventLoopRuntime, UvwTimerHandle, UvwWakeupBridge
│   │   └── bt_runtime/           BtRuntimeImpl, AsyncActionContextImpl, TreeInstanceImpl
│   ├── domain/
│   │   ├── entity/               EntityContextImpl
│   │   ├── group/                GroupContextImpl
│   │   └── world/                WorldSnapshotImpl, WorldSnapshotProviderImpl
│   ├── adapters/                 InProcessCommandBus
│   ├── bt_nodes/                 AsyncActionBase 实现
│   └── sim_host/                 SimHostApp + main.cpp（进程入口）
│
├── tests/                        GoogleTest 测试套件
│   ├── test_cancellation_token.cpp          # ICancellationToken 单元测试
│   ├── test_result_mailbox.cpp              # IResultMailbox 单元测试
│   ├── test_entity_context.cpp              # IEntityContext 单元测试
│   ├── test_async_action_base.cpp           # AsyncActionBase 单元测试
│   └── test_cross_library_integration.cpp  # 跨库边界集成测试（TBB↔Mailbox↔uvw）
│
├── scripts/
│   └── vendor-deps.sh            联网机器一键下载所有 zip 依赖
│
├── third_party/
│   ├── corekit/                  git submodule（必须，含 GlobalAllocator 等）
│   ├── oneTBB.zip                ← scripts/vendor-deps.sh 生成
│   ├── libuv.zip
│   ├── uvw.zip
│   ├── BehaviorTree.CPP.zip
│   └── googletest.zip
│
└── docs/design/
    ├── architecture.md           本文档
    └── behaviorTree+onetbb+uvw.md 原始设计文档
```

---

## 11. 依赖版本锁定

| 依赖 | 版本 | CMake 目标 | zip 来源 |
|------|------|-----------|---------|
| corekit | main (submodule) | `corekit` | git submodule 必须 |
| oneTBB | v2022.0.0 | `TBB::tbb` | [GitHub](https://github.com/oneapi-src/oneTBB/archive/refs/tags/v2022.0.0.zip) |
| libuv | v1.48.0 | `uv::uv` | [GitHub](https://github.com/libuv/libuv/archive/refs/tags/v1.48.0.zip) |
| uvw | v3.4.0_libuv_v1.48 | `uvw::uvw` | [GitHub](https://github.com/skypjack/uvw/archive/refs/tags/v3.4.0_libuv_v1.48.zip) |
| BehaviorTree.CPP | 4.9.0 | `BT::behaviortree_cpp` | [GitHub](https://github.com/BehaviorTree/BehaviorTree.CPP/archive/refs/tags/4.9.0.zip) |
| GoogleTest | v1.16.0 | `GTest::gtest` | [GitHub](https://github.com/google/googletest/archive/refs/tags/v1.16.0.zip) |

CMake 最低版本：**3.24** | C++ 标准：**C++17**

---

## 12. 开发阶段路线图

### Phase 1 — 最小闭环（当前）

目标：单实体、单树、一个 CPU 异步节点跑通完整链路。

- [ ] `ComputeAction::OnStart()` 提交 TBB 任务
- [ ] TBB worker 写入 `ResultMailbox`
- [ ] uvw `async_handle` 唤醒 BT re-tick
- [ ] `OnRunning()` 消费结果，返回 `SUCCESS`
- [ ] `OnHalted()` 取消任务，令牌生效

### Phase 2 — 实体级运行时

- [ ] `EntityContext` + `WorldSnapshot` 完整集成
- [ ] 多实体并行 tick（同一线程，串行）
- [ ] 编队协调（`GroupContext` 共享）

### Phase 3 — 总线接入

- [ ] uvw TCP/UDP `BusAdapter`
- [ ] `CommandBus` 接仿真宿主

### Phase 4 — 性能治理

- [ ] 多 arena 优先级验证
- [ ] 实体分组批量 tick
- [ ] 每帧 tick 耗时采样
- [ ] `TraceLogger`（节点状态迁移日志）
