# sim-behavior 架构设计

## 1. 设计目标

- **行为树语义与执行分离**：BehaviorTree.CPP 负责"该做什么"，oneTBB 负责"谁来算"，uvw 负责"什么时候回来"
- **线程边界清晰**：三类执行域不允许跨域直接操作共享状态
- **BT 节点可单元测试**：节点只依赖接口 Facade（IAsyncActionContext），不直接持有 uvw/TBB 原始对象
- **跨平台 & 内网部署**：所有依赖从源码编译，不依赖系统安装包

---

## 2. 六层架构

```
┌───────────────────────────────────────────────────────────────┐
│  Layer 1 — Simulation Host                                    │
│  sim_host/SimHostApp                                          │
│  职责：进程生命周期、配置加载、模块装配、时钟驱动、对外总线接入 │
└───────────────────────────────────────────────────────────────┘
            │ 装配 & 驱动
┌───────────────────────────────────────────────────────────────┐
│  Layer 2 — Behavior Runtime                                   │
│  runtime/bt_runtime/BtRuntimeImpl                             │
│  职责：BT 工厂、节点注册、树实例管理、TickScheduler、wakeup队列│
│  依赖库：BehaviorTree.CPP 4.6                                 │
└───────────────────────────────────────────────────────────────┘
            │ StartTimeout / EmitWakeup
┌───────────────────────────────────────────────────────────────┐
│  Layer 3 — Async Orchestration Layer (uvw)                    │
│  runtime/async_runtime/UvwEventLoopRuntime                    │
│  职责：事件循环、一次性/重复定时器、跨线程 PostToLoop、WakeupBridge│
│  依赖库：uvw v3.4 (header-only) → libuv v1.48                │
└───────────────────────────────────────────────────────────────┘
            │ SubmitCpuJob / Post(result)
┌───────────────────────────────────────────────────────────────┐
│  Layer 4 — Compute Execution Layer (oneTBB)                   │
│  runtime/compute_runtime/TbbJobExecutor                       │
│  三个 task_arena（high/normal/low）+ DefaultResultMailbox     │
│  依赖库：oneTBB v2022  TBB::tbb                               │
└───────────────────────────────────────────────────────────────┘
            │ 读写
┌───────────────────────────────────────────────────────────────┐
│  Layer 5 — Domain State Layer                                 │
│  domain/{entity,group,world}                                  │
│  EntityContext（实体私有状态）                                  │
│  GroupContext（编队共享状态）                                   │
│  WorldSnapshot（全局只读态势，每帧生成，BT 只读）              │
└───────────────────────────────────────────────────────────────┘
            │ Dispatch(ActionCommand)
┌───────────────────────────────────────────────────────────────┐
│  Layer 6 — Integration Adapters                               │
│  adapters/CommandBus, IBusAdapter                             │
│  职责：命令出站、仿真总线接入、传感器/武器系统适配             │
└───────────────────────────────────────────────────────────────┘
```

---

## 3. 三类线程域

| 线程域 | 线程载体 | 规则 |
|--------|----------|------|
| **BT Tick Domain** | SimHostApp 主循环线程 | 行为树 tick、黑板读写，单线程串行 |
| **uvw Event Domain** | UvwEventLoopRuntime 独立线程 | timer 回调、PostToLoop 回调，loop 内串行 |
| **TBB Compute Domain** | oneTBB worker 线程池 | CPU 密集计算，多线程并发 |

**跨域通信规则（强制）**：

```
TBB worker ──Post(result)──▶ DefaultResultMailbox
                                     │
                            notify_cb() → uvw async_handle.send()
                                     │
                        uvw loop 回调 → BtRuntime.RequestWakeup(entity_id)
                                     │
                     下一帧 TickAll() ← BT Tick Domain 消费结果
```

TBB worker **绝不**直接：
- 修改 BT 黑板
- 调用 EntityContext 的 set* 方法
- 调用 BtRuntime::TickEntity

---

## 4. 核心接口关系

```
IAsyncActionContext          ← BT 节点唯一依赖的运行时 Facade
  │
  ├── SubmitCpuJob() ──────▶ IJobExecutor (TbbJobExecutor)
  │                               │
  │                          IJobHandle (TbbJobHandle)
  │                          IResultMailbox (DefaultResultMailbox)
  │
  ├── StartTimeout() ──────▶ IEventLoopRuntime (UvwEventLoopRuntime)
  │                               │
  │                          ITimerHandle (UvwTimerHandle)
  │
  └── EmitWakeup() ─────────▶ IBtRuntime.RequestWakeup()
```

### AsyncActionBase（节点基类）

继承自 `BT::StatefulActionNode`，转发到三个虚方法：

| BT 回调 | 转发到 | 典型操作 |
|---------|--------|----------|
| `onStart()` | `OnStart()` | `SubmitCpuJob()` + `StartTimeout()` |
| `onRunning()` | `OnRunning()` | `PeekResult()` → 返回 SUCCESS/FAILURE |
| `onHalted()` | `OnHalted()` | `CancelJob()` + `CancelTimeout()` |

超时自动处理：`onRunning()` 先检查 `IsTimedOut()`，若已超时直接调用 `OnHalted()` 并返回 FAILURE。

---

## 5. 节点分类与实现策略

| 节点类型 | 场景示例 | 实现方式 | 备注 |
|----------|----------|----------|------|
| 同步条件节点 | HasTarget, IsAmmoSufficient | `BT::ConditionNode` 直接 tick | 不走 TBB/uvw，必须微秒级完成 |
| 同步瞬时动作 | SetFlag, ResetTimer | `BT::SyncActionNode` | 同上 |
| CPU 密集异步动作 | PathPlan, ThreatAssess | `AsyncActionBase` + TBB | `onStart` 提交 arena 任务 |
| I/O 等待型动作 | WaitBusEvent, WaitCommand | `AsyncActionBase` + uvw timer | `onStart` 注册 loop handle |
| 混合型动作 | PathPlanWithTimeout | `AsyncActionBase` + TBB + uvw | TBB 算，uvw 超时 |

---

## 6. oneTBB Arena 配置

三个 arena 对应三种 JobPriority，任务不跨 arena 执行：

| Arena | JobPriority | 并发数 | 典型任务 |
|-------|-------------|--------|----------|
| `arena_high_` | kHigh | 2 | 火力分配、威胁评估（战术决策） |
| `arena_normal_` | kNormal | 4 | 路径规划、地形查询、几何计算 |
| `arena_low_` | kLow | 2 | 预取、后台统计、离线缓存 |

> **注意**：`reserved_for_masters` 不要配置过高，否则 `enqueue()` 的调度保证会失效（oneTBB 官方文档警告）。

---

## 7. 黑板分层设计

```
┌─────────────────────────────────────────────────────────┐
│  EntityBlackboard（BT::Blackboard，per-entity）          │
│  短生命周期：当前目标、最近规划结果引用、局部状态标志      │
├─────────────────────────────────────────────────────────┤
│  GroupContext（C++ 对象，per-group）                      │
│  中生命周期：编队目标区、集结点、协调规则                   │
├─────────────────────────────────────────────────────────┤
│  WorldSnapshot（只读，per-frame）                         │
│  每帧刷新：全局态势、目标池、观测汇总                      │
│  BT 只读取，不写入                                        │
└─────────────────────────────────────────────────────────┘
```

---

## 8. 数据流（标准帧序列）

```
1. SimHostApp::TickLoop() — 帧开始（20 Hz，50ms/frame）
2. WorldSnapshotProvider::Refresh(sim_time)
3. BtRuntime::TickAll(sim_time)
   ├── 处理 wakeup_queue_（异步结果就绪的实体）
   └── 逐实体 tree->Tick()
       ├── 同步条件节点 → 直接读取 WorldSnapshot/EntityContext
       └── 异步动作节点
           ├── onStart()  → SubmitCpuJob() → TBB arena 入队
           └── onRunning() → PeekResult() → 若就绪返回 SUCCESS/FAILURE
4. TBB worker 计算完成
   └── mailbox.Post(result) → notify_cb() → uvw async_handle.send()
5. uvw loop 回调
   └── DrainAll() → BtRuntime.RequestWakeup(entity_id)
6. 下一帧 TickAll() 处理 wakeup_queue_，节点消费结果
```

---

## 9. 依赖版本锁定

| 依赖 | 锁定版本 | 引入方式 | 目标名 |
|------|---------|----------|--------|
| corekit | main branch submodule | git submodule | `corekit` |
| oneTBB | v2022.0.0 | submodule / FetchContent | `TBB::tbb` |
| libuv | v1.48.0 | submodule / FetchContent | `uv::uv` |
| uvw | v3.4.0_libuv_v1.48 | submodule / FetchContent | `uvw::uvw` |
| BehaviorTree.CPP | 4.6.2 | submodule / FetchContent | `BT::behaviortree_cpp` |
| GoogleTest | v1.14.0 | submodule / FetchContent | `GTest::gtest` |

所有依赖均从源码编译，不依赖系统安装包，保证在内网环境可完整构建。

---

## 10. 开发阶段路线图

### Phase 1 — 最小闭环（当前）

目标：单实体、单树、一个 CPU 异步节点跑通完整链路。

验证点：
- [ ] PathPlanAction::OnStart() 提交 TBB 任务
- [ ] TBB worker 写入 ResultMailbox
- [ ] uvw async_handle 唤醒 BT re-tick
- [ ] OnRunning() 消费结果，返回 SUCCESS
- [ ] OnHalted() 取消任务，令牌生效

### Phase 2 — 实体级运行时

- [ ] EntityContext + WorldSnapshot 完整集成
- [ ] 多实体并行 tick（同一线程，串行）
- [ ] 编队协调（GroupContext 共享）

### Phase 3 — 总线接入

- [ ] uvw TCP/UDP BusAdapter
- [ ] CommandBus 接仿真宿主

### Phase 4 — 性能治理

- [ ] 多 arena 优先级验证
- [ ] 实体分组批量 tick
- [ ] 每帧 tick 耗时采样
- [ ] TraceLogger（节点状态迁移日志）
