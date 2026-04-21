# Squad Patrol 示例

这个示例演示 `sim-behavior` 在一个小型多实体巡逻场景中的完整工作流：

- 读取 JSON 配置，决定实体数量、运行帧数和行为参数
- 加载 BehaviorTree XML，定义“扫描 → 巡逻 / 发现威胁 → 上报”的决策逻辑
- 注册自定义 BT 节点
- 为每个实体创建独立上下文，为整支小队创建共享 `GroupContext`
- 通过 `TBB + uvw + mailbox + wakeup` 链路驱动异步动作完成后重新 tick
- 在终端打印运行中的行为和帧统计

## 目录结构

```text
examples/squad_patrol/
├── CMakeLists.txt
├── README.md
├── config/
│   └── simulation.json
├── src/
│   ├── config_loader.hpp
│   ├── main.cpp
│   └── nodes.hpp
└── trees/
    └── squad_tree.xml
```

## 运行方式

在仓库根目录配置并构建：

```bash
cmake -S . -B build -DSIMBEHAVIOR_BUILD_EXAMPLES=ON
cmake --build build --config Release --target squad_patrol_example
```

从产物目录运行：

```bash
cd build/Release
./squad_patrol_example
```

也可以显式指定配置文件：

```bash
./squad_patrol_example config/simulation.json
```

Windows 下示例启动时会主动把控制台输入/输出代码页切到 UTF-8，避免中文和符号日志乱码。

## 入口流程

`src/main.cpp` 是示例总入口，执行顺序可以概括为：

1. 设置信号处理和控制台 UTF-8。
2. 通过 `LoadConfig()` 读取 `config/simulation.json`。
3. 构造并初始化 `SimHostApp`。
4. 根据配置设置 `TickPolicy`。
5. 把配置里的扫描时长、巡逻时长、上报时延、威胁概率写入 `nodes.hpp` 中的全局原子参数。
6. 注册所有 BT 节点类型。
7. 载入 `trees/squad_tree.xml` 对应的行为树模板。
8. 注册 `CommandBus` 处理器，模拟“指挥中心”接收 `threat_report`。
9. 为每个成员调用 `SpawnEntity()` 创建实体树实例。
10. 调用 `AssignGroup()` 创建共享编队上下文。
11. 进入固定 20Hz 的主循环，每帧调用 `BtRuntime().TickAll(sim_time)`。
12. 主循环结束后调用 `RequestStop()`，输出汇总统计。

## 配置文件说明

`config/simulation.json` 分为三部分：

- `simulation`
  - `name`：场景名，只用于展示。
  - `duration_frames`：运行总帧数。
  - `tick_policy`：`tick_all` 或 `skip_idle`。
- `squad`
  - `id` / `name`：编队 ID 和名称。
  - `objective` / `rally_point`：共享编队目标点和集结点。
  - `members`：成员列表，每个成员有 `id`、`name`、`zone`。
- `behavior`
  - `tree_file`：行为树 XML 路径。
  - `scan_duration_ms`：扫描任务耗时。
  - `patrol_step_ms`：巡逻移动任务耗时。
  - `report_duration_ms`：上报确认耗时。
  - `threat_probability_pct`：扫描时发现威胁的概率。

这些值不会直接改树结构，而是影响节点行为。

## 行为树逻辑

`trees/squad_tree.xml` 定义了一棵 `Fallback` 树：

```text
Fallback
├── Sequence(threat_response)
│   ├── ThreatDetectedCondition
│   └── ReportThreatAction
└── Sequence(patrol_cycle)
    ├── ScanZoneAction
    └── PatrolMoveAction
```

含义是：

- 先看当前实体是否已经发现威胁。
- 如果发现了，就优先进入上报分支。
- 如果没有发现威胁，就执行正常巡逻分支。

因此这是一个“威胁响应优先于日常巡逻”的结构。

## 节点职责

`src/nodes.hpp` 里定义了 5 种节点。

### 1. `ThreatDetectedCondition`

- 读取当前实体 `EntityContext["threat_detected"]`
- 为 `true` 时返回 `SUCCESS`
- 为 `false` 时返回 `FAILURE`

它决定实体是否进入威胁上报分支。

### 2. `SquadAlertCondition`

- 读取编队共享 `GroupContext["squad_alert"]`
- 示例树里没有实际用到
- 主要用于展示“编队级共享状态”也可以进入 BT 决策

### 3. `ScanZoneAction`

- `OnStart()`：
  - 提交一个 `TBB` 普通优先级任务，模拟传感器扫描
  - 启动超时计时器
- `OnRunning()`：
  - 非阻塞轮询结果邮箱
  - 如果发现威胁：
    - 设置本实体 `threat_detected=true`
    - 设置编队 `squad_alert=true`
  - 如果未发现威胁：仅打印“区域安全”
- `OnHalted()`：
  - 取消任务并清理超时

这个节点是“感知入口”。

### 4. `PatrolMoveAction`

- `OnStart()`：
  - 读取当前实体 `patrol_step`
  - 提交一个低优先级 `TBB` 任务，模拟移动到下一个路点
- `OnRunning()`：
  - 收到结果后写回新的 `patrol_step`
  - 更新全局巡逻统计
- `OnHalted()`：
  - 取消任务

这个节点是“正常巡逻推进器”。

### 5. `ReportThreatAction`

- `OnStart()`：
  - 立即通过 `CommandBus.Dispatch()` 派发 `threat_report`
  - 再提交一个高优先级 `TBB` 任务，模拟等待指挥部确认
- `OnRunning()`：
  - 任务完成后清除本实体 `threat_detected`
  - 同时清除编队 `squad_alert`
  - 更新上报统计
- `OnHalted()`：
  - 取消确认任务

这个节点把“发现威胁”转成“业务事件 + 状态回收”。

## 上下文模型

示例同时使用了三类上下文：

- `EntityContext`
  - 每个实体独有
  - 存放 `threat_detected`、`patrol_step` 之类的私有状态
- `GroupContext`
  - 小队成员共享
  - 存放 `squad_alert`、目标点、集结点、成员列表
- `AsyncActionContext`
  - 给异步节点使用
  - 负责提交任务、查询结果、管理超时、取消任务

节点通过 Blackboard 自动拿到这些上下文，而不是自己创建依赖。

## 异步执行链路

这个示例最关键的是异步动作如何“完成后唤醒树”。

完整链路如下：

1. `ScanZoneAction` / `PatrolMoveAction` / `ReportThreatAction` 在 `OnStart()` 中提交 `TBB` 任务。
2. 任务完成后把 `JobResult` 投递到 `ResultMailbox`。
3. mailbox 的通知回调把处理逻辑切到 `uvw` event loop 线程。
4. 在 event loop 线程中 `DrainAll()`，逐条调用 `BtRuntime::RequestWakeup(entity_id)`。
5. 下一帧 `TickAll()` 的 Phase 1 会优先处理被唤醒实体。
6. 节点在 `OnRunning()` 中读到结果，完成状态转换。

因此主线程不阻塞等待任务，而是下一帧自然续跑。

## 主循环行为

`main.cpp` 没有直接调用 `app.Run()`，而是自己维护一个固定 20Hz 循环，主要是为了每帧都能读取统计并打印。

每一帧做的事是：

1. `sim_time += 50ms`
2. 调用 `BtRuntime().TickAll(sim_time)`
3. 每隔 5 帧打印一次 `TickStats`
4. 睡眠到下一帧时间点

这样可以直观看到：

- 当前仿真时间
- 每帧 tick 耗时
- 树总数、跳过数、唤醒数
- 已发现威胁次数、已上报次数、已完成巡逻次数

## 日志与终端输出

示例有两类输出：

- `std::cout`
  - 打印 Banner、帧统计、节点执行过程
- `SimBtLog()/corekit/glog`
  - 打印底层运行时日志，例如 `BtRuntime`、`TbbJobExecutor`、`ResultMailbox`

当前日志前缀已经整理为“只保留一份真实调用点”，例如：

```text
I20260421 11:54:00.201272 50360 sim_host_app.cpp:37] SimHostApp: initializing
```

不会再出现 `log_manager.cpp:593] [sim_host_app.cpp:37]` 这种双文件位置前缀。

## 适合怎么改

如果你想继续扩展示例，最直接的切入点有三类：

- 改树结构
  - 直接修改 `trees/squad_tree.xml`
  - 例如把 `SquadAlertCondition` 加入树中，让非发现者成员也响应警报
- 改行为参数
  - 修改 `config/simulation.json`
  - 例如提高 `threat_probability_pct`，更容易观察上报路径
- 改节点逻辑
  - 修改 `src/nodes.hpp`
  - 例如让 `ReportThreatAction` 在收到确认前维持更久的警报状态

## 最小阅读顺序

如果你第一次接触这个示例，建议按这个顺序读：

1. `config/simulation.json`
2. `trees/squad_tree.xml`
3. `src/nodes.hpp`
4. `src/main.cpp`

这个顺序最容易先建立“场景参数 → 决策结构 → 节点行为 → 运行装配”的完整心智模型。
