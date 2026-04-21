# sim-behavior 前端可视化配置平台 — 需求设计文档

> 版本：v1.0 — Qt/C++ Desktop 版
> 适用范围：基于 sim-behavior 运行时的 Qt Widgets 桌面端可视化编辑器
> 技术路线：Qt 5.12+ (Widgets) + C++17 + CMake，与运行时同进程编译

---

## 1. 项目概述

### 1.1 背景

当前 `sim-behavior` 框架通过手写 XML/JSON/C++ 代码完成全部配置：
- 行为树定义：`trees/*.xml`
- 实体与仿真参数：`config/*.json`
- 自定义节点类型：C++ 头文件 + `RegisterAllNodes()`
- Bus 出站：代码里调用 `SetBusAdapter()`

本方案目标是构建一个**原生 Qt Widgets 桌面程序**，与 `sim-behavior` 运行时编译为同一进程，通过 C++ API 直接交互，为行为设计师、仿真工程师和测试人员提供可视化配置与监控能力。

### 1.2 核心架构决策

```
┌─────────────────────────────────────────────────────────────┐
│                    Qt Widgets Application                    │
│  ┌─────────────┐ ┌─────────────┐ ┌───────────────────────┐  │
│  │ QMainWindow │ │ QDockWidget │ │    QGraphicsScene     │  │
│  │  菜单/工具栏 │ │  属性/面板  │ │    行为树画布/2D态势   │  │
│  └──────┬──────┘ └──────┬──────┘ └───────────┬───────────┘  │
│         └────────────────┴────────────────────┘              │
│                         │                                    │
│              Qt Signal/Slot（跨线程安全通知）                  │
│                         │                                    │
│  ┌──────────────────────┴────────────────────┐               │
│  │         sim-behavior Runtime Layer         │               │
│  │  SimHostApp / IBtRuntime / ICommandBus     │               │
│  │  EntityContext / GroupContext / WorldSnap  │               │
│  └────────────────────────────────────────────┘               │
└─────────────────────────────────────────────────────────────┘
                         │
              同一进程，直接 C++ API 调用
              无需 HTTP / WebSocket / 序列化
```

**关键决策**：
- **同进程编译**：Qt UI 模块与 `sim-behavior` 各静态库链接为同一个可执行文件
- **直接 API 调用**：Qt 线程通过 `SimHostApp` 的公有方法操作运行时，无需网络层
- **Qt 信号槽做异步通知**：`SimHostApp` 的运行时事件（TBB 完成、Tick 结束、状态变更）通过 Qt 信号通知 UI 线程

### 1.3 用户角色

| 角色 | 主要职责 | 核心使用模块 |
|---|---|---|
| **行为设计师** | 设计 AI 行为逻辑、节点参数、树结构 | 行为树编辑器、黑板变量设计器 |
| **仿真工程师** | 配置实体、编队、运行仿真、观察状态 | 实体编排面板、仿真控制台、状态监控 |
| **系统集成师** | 配置外部总线、网络协议、命令路由 | Bus 配置中心、Adapter 管理器 |
| **测试人员** | 验证逻辑、单步调试、日志分析 | 日志回放器、树执行追踪、变量对比 |

---

## 2. 功能模块总览

```
┌─────────────────────────────────────────────────────────────┐
│  QMainWindow（主窗口）                                        │
│  ├── QMenuBar：文件 / 编辑 / 仿真 / 视图 / 帮助               │
│  └── QToolBar：新建树 / 运行 / 暂停 / 单步 / 适配器状态       │
├─────────────────────────────────────────────────────────────┤
│  Central Widget（QStackedWidget 切换核心工作区）              │
│  ├── Page 1: 行为树编辑器（QGraphicsView + QDockWidget）      │
│  ├── Page 2: 实体编排中心（QTreeView + QTableView）           │
│  ├── Page 3: 总线配置中心（QTabWidget + 表单）                │
│  └── Page 4: 仿真监控台（QSplitter + 自定义绘图控件）          │
├─────────────────────────────────────────────────────────────┤
│  Dock Widgets（可浮动/停靠）                                  │
│  ├── 节点库面板（QTreeWidget）                                │
│  ├── 属性编辑器（QFormLayout / 自定义 PropertyGrid）          │
│  ├── 黑板变量面板（QTreeView + QAbstractItemModel）           │
│  ├── 实体状态面板（QListView / 自定义 EntityCard）            │
│  ├── 消息调试器（QTableView + 过滤栏）                        │
│  └── 日志输出（QPlainTextEdit + 语法高亮）                    │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. 模块详细设计

---

### 模块一：行为树可视化编辑器（Tree Editor）

#### 3.1.1 画布：`QGraphicsScene` + `QGraphicsView`

- **自定义图元**：
  - `BTNodeItem`（继承 `QGraphicsItem`）：绘制节点矩形，显示节点类型、名称、状态色块
  - `BTEdgeItem`（继承 `QGraphicsLineItem`）：贝塞尔曲线连接父子节点
  - `BTPortItem`（继承 `QGraphicsEllipseItem`）：节点左右两侧的端口圆点，用于黑板变量连线的锚点
- **交互操作**：
  - 左键拖拽节点移动（`mouseMoveEvent` + `setPos`）
  - 右键菜单：删除节点、复制节点、强制返回 SUCCESS/FAILURE（调试时）
  - 滚轮缩放画布（`QGraphicsView::setTransformationAnchor` + `scale`）
  - 中键平移画布（自定义 `mousePressEvent` 记录起始点，`mouseMoveEvent` 滚动）
  - 从节点库面板 `QTreeWidget` 拖拽到画布创建节点（实现 `mimeData` / `dropEvent`）
- **自动布局**：集成 `dagre` C++ 移植版或手写树形布局算法（按层级计算 x/y），菜单栏提供"自动排列"按钮
- **多树标签页**：`QTabBar` + 每个标签对应一棵独立的 `QGraphicsScene`

#### 3.1.2 节点库面板（左侧 Dock：`QDockWidget` + `QTreeWidget`）

```
节点库
├── 控制节点
│   ├── Sequence
│   ├── Fallback
│   ├── Parallel
│   └── ReactiveSequence
├── 条件节点（动态加载自后端 Registry）
│   ├── ThreatDetectedCondition
│   └── SquadAlertCondition
├── 同步动作
├── 异步动作
│   ├── ScanZoneAction
│   ├── PatrolMoveAction
│   └── ReportThreatAction
└── 装饰器
    ├── Retry
    ├── Timeout
    └── Inverter
```

- **动态加载**：应用启动时调用 `IBtRuntime::GetRegisteredNodeTypes()`（需后端新增此方法）或读取预生成的 JSON 描述文件，填充 `QTreeWidget`
- **拖拽支持**：`QTreeWidgetItem` 设置 `Qt::ItemIsDragEnabled`，携带 MIME 类型 `application/x-bt-node-type`

#### 3.1.3 属性编辑器（右侧 Dock：`QDockWidget` + `QFormLayout`）

点击画布节点后，属性面板动态生成表单：

**通用属性**：
| 控件 | 类型 | 对应 XML 属性 |
|---|---|---|
| `QLineEdit` | name | `name="soldier_decision"` |
| `QTextEdit` | comment | 生成到 XML 注释 |
| `QLabel` | node_type 只读 | `ScanZoneAction` |

**异步动作特有**：
| 控件 | 类型 | 对应代码 |
|---|---|---|
| `QComboBox` | Job Priority | `JobPriority::kHigh/Normal/Low` |
| `QSpinBox` | Timeout (ms) | `StartTimeout()`，0 表示无超时 |
| `QSpinBox` | Retry Count | 装饰器 `Retry` 的 `num_attempts` |

**条件节点特有**：
| 控件 | 类型 | 说明 |
|---|---|---|
| `QCheckBox` | Invert Result | 外包 `Inverter` 装饰器的快捷选项 |

**端口绑定区域**（属性面板下半部分）：
- `QTableWidget`，列：`Port Name`, `Direction`, `Bound To`, `Type`
- 双击 `Bound To` 列弹出对话框：
  - 选项 A：绑定到黑板变量（下拉选择现有变量或新建）
  - 选项 B：固定常量（弹出输入框，按 `data_type` 校验）
  - 选项 C：表达式（`QLineEdit`，如 `"pos.x + 10"`）

#### 3.1.4 黑板变量面板（左下 Dock）

- `QTreeView` + 自定义 `QAbstractItemModel`（`BlackboardModel`）
- 列：变量名、类型、初始值、被引用数
- 工具栏按钮：添加变量、删除变量、修改变量类型
- **引用高亮**：选中变量后，通过 `QGraphicsScene::update()` 重绘画布，引用该变量的节点边框加粗发光（`QGraphicsDropShadowEffect`）

#### 3.1.5 XML 预览与文件操作

- 底部 Dock：`QPlainTextEdit`（只读，Monaco 风格等宽字体），实时同步当前树的 XML 文本
- **菜单栏操作**：
  - `文件 → 新建树`：`QInputDialog` 输入 Tree ID
  - `文件 → 导入 XML...`：`QFileDialog` 选择 `.xml` → 解析并还原画布
  - `文件 → 导出 XML...`：`QFileDialog` 保存
  - `文件 → 校验`：检查孤立节点、必填端口未绑定、循环引用 → `QMessageBox` 显示结果

---

### 模块二：实体编排中心（Entity Orchestrator）

#### 3.2.1 实体列表：`QTableView` + `QAbstractTableModel`

- 模型列：
  | # | 显示列 | 编辑器 | 说明 |
  |---|---|---|---|
  | 0 | Entity ID | `QSpinBox` | 唯一标识 |
  | 1 | Name | `QLineEdit` | 显示名 |
  | 2 | Assigned Tree | `QComboBox` | 下拉选择已加载的树 ID |
  | 3 | Group | `QComboBox` | 下拉选择已创建的编队 |
  | 4 | Status | `QLabel`（只读） | 运行时状态，文字+图标 |
  | 5 | Position | `QLineEdit`（x,y,z）| Vec3 字符串表示 |

- **操作按钮**（工具栏或右键菜单）：
  - 新增实体（`+`）
  - 复制选中实体（复制配置，新 ID 自动递增）
  - 删除实体（`-`）
  - 批量导入 JSON / CSV（`QFileDialog`）

#### 3.2.2 实体详情：右侧 Dock 或 `QDialog`

选中实体后，右侧面板切换为详情表单：

```
┌─ 基础属性 ───────────────┐
│ Entity ID:   [101     ]  │
│ Name:        [Alpha-1 ]  │
│ Tree:        [v SoldierPatrolTree] │
│ Group:       [v Alpha Squad #1001] │
│ Position:    [0, 0, 0]   │
├─ EntityContext 初始值 ───┤
│ [+] threat_detected  bool  false   │
│ [+] patrol_step      int   0       │
│ [+] ammo_count       int   30      │
└──────────────────────────┘
```

- `EntityContext` 编辑器：自定义 `KeyValueEditor` Widget，支持添加/删除/修改行，类型下拉（bool/int/float/string/Vec3）
- 数据存在前端内存模型中，点击"保存到项目"后写入项目配置，点击"应用到运行时"则调用 `SimHostApp::SpawnEntity()`

#### 3.2.3 编队管理：`QGroupBox` + 拖拽分配

- **左侧面板**：`QListWidget` 展示所有编队卡片（`GroupCardWidget`），显示 Group ID、Name、成员数量
- **成员分配**：
  - 方式一：在实体列表中多选行，右键"加入编队" → 选择目标编队
  - 方式二：拖拽实体列表行到编队卡片上（实现 `dragEnterEvent` / `dropEvent`）
- **编队属性编辑器**（点击编队卡片后）：
  | 字段 | 控件 | 对应代码 |
  |---|---|---|
  | Group ID | `QSpinBox` | `AssignGroup(group_id, ...)` |
  | Name | `QLineEdit` | 前端展示 |
  | Objective | `QLineEdit` (x,y,z) | `group_ctx->SetObjectivePosition()` |
  | Rally Point | `QLineEdit` (x,y,z) | `group_ctx->SetRallyPoint()` |
  | Rules | `KeyValueEditor` | `group_ctx->SetRule()` |

#### 3.2.4 配置导入/导出

- **导出 JSON**：`QFileDialog` 选择路径 → 生成与 `config/simulation.json` 格式兼容的文件
- **导入 JSON**：读取 JSON → 解析并填充实体列表和编队列表
- **热重载按钮**：对运行中的 `SimHostApp` 调用 `DespawnEntity` + `SpawnEntity` 实现动态刷新（需后端支持）

---

### 模块三：总线配置中心（Bus Configuration Center）

#### 3.3.1 本地路由配置：`QTableWidget`

- 列：Topic、Handler 数量、Handler 类型（C++ / Script）
- 双击行打开 `HandlerEditorDialog`：
  - 如果是 Script 类型，显示 `QPlainTextEdit` 编写 JS-like 脚本（用于前端本地调试用，不进入后端运行时）
  - 实际 C++ Handler 只能查看名称和源码位置，不能在前端修改（因为需重编译）

#### 3.3.2 出站适配器配置：`QTabWidget`

每个适配器一个标签页，页面内为表单布局：

```
┌─ Adapter: CommandCenterUdp ─────────┐
│ Protocol:        [UDP          v]   │
│ Local Port:      [0            ]    │
│ Remote Host:     [192.168.1.100]    │
│ Remote Port:     [8888         ]    │
│ Enabled Topics:  [☑] threat_report   │
│                  [☑] engage_target   │
│                  [ ] move_to         │
│ Encoding:        [内置二进制   v]   │
│                                     │
│ [测试连接] [保存并启用] [删除适配器] │
└─────────────────────────────────────┘
```

- **测试连接**：前端构造一个探测用的 `BusMessage`，通过 `UvwUdpBusAdapter::Publish()` 发送，等待超时或收到回环响应，显示 `QMessageBox` 结果
- **保存并启用**：调用 `InProcessCommandBus::SetBusAdapter(adapter, event_loop)` 并持久化配置到项目文件
- **状态指示灯**：表单右上角放置 `QLabel` + 圆形 `QWidget`（绿色=已连接，红色=未连接，黄色=错误）

#### 3.3.3 入站映射配置：`QTableWidget`

| 入站 Topic | 目标类型 | 目标 ID | 操作 | 参数 |
|---|---|---|---|---|
| `external.fire_cmd` | Group | 1001 | SetRule | `fire_authorized=true` |
| `external.relocate` | Entity | 101 | SetFlag | `relocation_target=Vec3` |

- 操作列下拉：写 EntityContext / 写 GroupContext / RequestWakeup / 记录日志
- 保存后，前端在收到对应入站消息时自动执行映射（如果前端负责监听 UDP），或生成配置让后端加载

#### 3.3.4 消息调试器（Dock Widget）

- **上半部分**：手动发送区
  - `QComboBox` 选择 Topic，`QLineEdit` 填 Payload（Hex/文本），`QSpinBox` 选 Source Entity
  - "发送"按钮调用 `ICommandBus::Dispatch(cmd)`
- **下半部分**：消息监听表格 `QTableView`
  - 列：时间戳、方向（IN/OUT）、Topic、Source Entity、Payload（截断显示）
  - 自动滚动到底部（`scrollToBottom`）
  - 顶部过滤栏：按 Topic 下拉过滤、按实体 ID 过滤、按时间范围过滤

---

### 模块四：仿真控制台（Simulation Control）

#### 3.4.1 主工具栏控制按钮

```cpp
// 使用 QToolBar + QAction + QIcon
actionStart_ = new QAction(QIcon(":/icons/play.png"), "启动仿真", this);
actionPause_ = new QAction(QIcon(":/icons/pause.png"), "暂停", this);
actionStep_  = new QAction(QIcon(":/icons/step.png"), "单步 Tick", this);
actionStop_  = new QAction(QIcon(":/icons/stop.png"), "停止", this);
```

- **启动**：在独立 `QThread` 中调用 `SimHostApp::Run()`，避免阻塞 UI 主线程
- **暂停**：设置原子标志 `pause_requested_`，在 `TickLoop` 中检查并进入等待状态
- **单步**：执行一次 `TickAll(sim_time)` 后立即返回，用于精确调试
- **停止**：调用 `SimHostApp::RequestStop()`

#### 3.4.2 Tick 策略与速率

- `QComboBox`：策略切换 `kSkipIdle` / `kTickAll` → 调用 `SimHostApp::SetTickPolicy()`
- `QSlider`：速率倍率 0.1x ~ 5x → 调整 `TickLoop` 的 `sleep_for` 时长（`base_interval / rate`）

#### 3.4.3 性能面板（Dock Widget，使用自定义绘图）

- **Tick 耗时折线图**：继承 `QWidget`，重写 `paintEvent()`，用 `QPainter` 绘制最近 200 帧的 `TickStats.duration_us`
- **实体状态饼图**：RUNNING / IDLE / WAKEUP 的数量占比，用 `QPainter::drawPie` 绘制
- **TBB 队列长度**：三个 `QProgressBar` 分别显示 `arena_high_`, `arena_normal_`, `arena_low_` 的 pending 任务数
- **Bus 流量柱状图**：每秒按 Topic 分布的消息数

#### 3.4.4 日志输出：`QPlainTextEdit`

- 自定义 `LogHighlighter`（继承 `QSyntaxHighlighter`）：INFO 为黑色，WARN 为橙色，ERROR 为红色
- 容量限制：保留最近 5000 行，超出时移除头部
- 搜索框：`QLineEdit` + `QPushButton("查找下一个")`，高亮匹配文本
- 点击包含 `EntityId=xxx` 的日志，自动在实体面板中选中对应实体（通过正则解析）

---

### 模块五：数据流与状态可视化

#### 3.5.1 黑板数据流图（Blackboard Flow Graph）

- **同一 `QGraphicsScene` 的叠加层**：在行为树编辑器画布上增加半透明连线
- 每个 `BTNodeItem` 知道自己读写了哪些黑板变量（通过解析 `ports`）
- 绘制从"写入节点"到"读取节点"的虚线箭头（`QGraphicsPathItem` + `QPen(Qt::DashLine)`）
- 鼠标悬停变量名时，相关连线和节点高亮

#### 3.5.2 Entity 状态监控：`QListView` + 自定义委托

- `EntityCardDelegate`（继承 `QStyledItemDelegate`）：在列表项中绘制卡片
  - 顶部：Entity ID + Name + 状态图标（绿色/红色/蓝色圆点）
  - 中部：当前 RUNNING 节点名称
  - 底部：3 个关键 EntityContext 值（可配置显示哪些 key）
- **告警动画**：当 `threat_detected=true` 时，卡片边框用 `QPropertyAnimation` 做红色脉冲动画
- **刷新频率**：每 5 帧 Tick 更新一次（约 250ms），避免 UI 过度刷新

#### 3.5.3 Group 状态面板

- `QGroupBox` 列表，每个编队一个盒子
- 内部用 `QFormLayout` 展示 Rules（开关用 `QCheckBox`，只读）
- 成员列表用 `QListWidget` 展示，点击成员可跳转到对应实体卡片

#### 3.5.4 世界态势 2D 视图：`QGraphicsView`

- **独立 Scene**：`WorldScene : public QGraphicsScene`
- 图元：
  - `EntityGraphicsItem`：不同图标（士兵、无人机、车辆），颜色根据状态变化
  - `GroupLinkItem`：编队成员间的虚线连接
  - `ThreatGraphicsItem`：红色闪烁的感叹号图标
  - `ObjectiveMarker`：黄色旗帜图标（Objective / Rally Point）
- 交互：滚轮缩放、中键平移、左键点击实体选中并弹出信息气泡
- **时间轴回放**：底部 `QSlider`，拖动时显示历史 `WorldSnapshot`（需后端保存最近 N 帧快照）

---

## 4. C++ 数据模型设计

前端不采用 JSON/TypeScript，而是直接定义 C++ 结构体，与运行时对象无缝交互。

```cpp
// models/project_model.hpp
#pragma once
#include <QString>
#include <QVector>
#include <QMap>
#include <QVariant>
#include <QGraphicsItem>

namespace sim_bt { namespace ui {

struct Vec3 {
    float x = 0, y = 0, z = 0;
    QString toString() const { return QString("%1,%2,%3").arg(x).arg(y).arg(z); }
};

struct PortBinding {
    QString portName;
    enum Direction { Input, Output } direction;
    enum BindType { BlackboardKey, Constant, Expression } bindType;
    QString boundValue;  // key name, constant literal, or expression string
};

struct BTNodeModel {
    QString id;           // UUID，前端生成
    QString type;         // "ScanZoneAction", "Sequence" ...
    QString category;     // "control", "condition", "sync_action", "async_action", "decorator"
    QString instanceName;
    QMap<QString, QVariant> parameters;  // priority, timeout_ms, retry_count ...
    QVector<PortBinding> ports;
    QPointF canvasPos;
};

struct BTEdgeModel {
    QString fromNodeId;
    QString toNodeId;
    int childIndex = 0;   // 对于 Sequence/Fallback，子节点的顺序索引
};

struct BlackboardVariable {
    QString key;
    QString type;         // "bool", "int", "float", "string", "Vec3", "EntityId"
    QVariant initialValue;
    enum Scope { TreeScope, EntityScope, GlobalScope } scope;
};

struct BehaviorTreeModel {
    QString treeId;       // XML 中的 ID，如 "SoldierPatrolTree"
    QString displayName;
    QVector<BTNodeModel> nodes;
    QVector<BTEdgeModel> edges;
    QVector<BlackboardVariable> blackboard;

    QString toXml() const;     // 序列化为 BehaviorTree.CPP v4 XML
    static std::optional<BehaviorTreeModel> fromXml(const QString& xml);
};

struct EntityConfigModel {
    uint32_t entityId = 0;
    QString displayName;
    QString treeId;       // 绑定的行为树
    uint32_t groupId = 0; // 0 表示未分配编队
    Vec3 initialPosition;
    QMap<QString, QVariant> initialContext;  // EntityContext 初始值
};

struct GroupConfigModel {
    uint32_t groupId = 0;
    QString name;
    QVector<uint32_t> memberIds;
    Vec3 objectivePosition;
    Vec3 rallyPoint;
    QMap<QString, QVariant> initialRules;
};

struct OutboundAdapterConfig {
    QString id;
    QString name;
    QString protocol;          // "udp", "tcp", "http", "grpc"
    uint16_t localPort = 0;
    QString remoteHost;
    uint16_t remotePort = 0;
    QVector<QString> enabledTopics;
    QString encoding;          // "builtin_binary", "json", "protobuf"
};

struct ProjectModel {
    QString projectName;
    QString rootPath;          // 关联的 sim-behavior 项目目录
    QVector<BehaviorTreeModel> trees;
    QVector<EntityConfigModel> entities;
    QVector<GroupConfigModel> groups;
    QVector<OutboundAdapterConfig> outboundAdapters;

    void saveToDisk() const;
    static std::optional<ProjectModel> loadFromDisk(const QString& path);
};

}} // namespace
```

---

## 5. 前后端集成架构（同进程直接调用）

由于前端与后端是同一进程，**不需要 HTTP / WebSocket / 序列化层**。

### 5.1 架构层次

```
┌────────────────────────────────────────────┐
│  Qt UI Layer (主线程)                       │
│  ├── MainWindow (QMainWindow)              │
│  ├── TreeEditorWidget (QGraphicsView)      │
│  └── ...                                   │
│       │                                    │
│       ▼ 直接 C++ API 调用（同步）           │
│  SimHostAppFacade (封装层)                  │
│       │                                    │
│       ▼ 持有 SimHostApp* 指针               │
│  sim-behavior Runtime Layer                 │
│  ├── SimHostApp                            │
│  ├── BtRuntimeImpl                         │
│  └── ...                                   │
└────────────────────────────────────────────┘
         │
         │ Qt Signal/Slot（跨线程异步通知）
         │
┌────────────────────────────────────────────┐
│  Runtime Worker Thread (QThread)            │
│  ├── TickLoop 运行在此线程                  │
│  ├── TBB Worker Threads (非 Qt)             │
│  └── uvw Loop Thread (非 Qt)                │
└────────────────────────────────────────────┘
```

### 5.2 核心封装类：`SimHostAppFacade`

为了避免 Qt UI 直接耦合 `sim_host_app.hpp`，新增一个轻量封装层，负责：
1. 将运行时事件翻译为 Qt 信号
2. 将 UI 操作翻译为运行时调用
3. 线程安全桥接（运行时事件可能来自非 Qt 线程）

```cpp
// ui/runtime_facade.hpp
#pragma once
#include <QObject>
#include <QThread>
#include "sim_host/sim_host_app.hpp"

namespace sim_bt { namespace ui {

class SimHostAppFacade : public QObject {
    Q_OBJECT
public:
    explicit SimHostAppFacade(QObject* parent = nullptr);
    ~SimHostAppFacade();

    // ── 同步调用（UI 线程直接调用，快速返回）──
    bool initializeRuntime();
    bool spawnEntity(uint32_t entityId, const QString& treeName);
    bool despawnEntity(uint32_t entityId);
    bool assignGroup(uint32_t groupId, const QVector<uint32_t>& members);
    void setTickPolicy(IBtRuntime::TickPolicy policy);
    void dispatchCommand(const ActionCommand& cmd);  // 调试用
    ProjectModel currentProject() const;

    // ── 仿真控制（启动独立线程）──
    void startSimulation();   // 在新 QThread 中调用 SimHostApp::Run()
    void pauseSimulation();
    void stepSimulation();    // 单步 tick
    void stopSimulation();

    // ── 状态查询（UI 定时器轮询或信号驱动）──
    IBtRuntime::TickStats lastTickStats() const;
    EntityRuntimeState getEntityState(uint32_t entityId) const;
    QVector<EntityRuntimeState> getAllEntityStates() const;

signals:
    // 以下信号通过 Qt::QueuedConnection 从运行时线程安全投递到 UI 线程
    void entityStateUpdated(uint32_t entityId, const EntityRuntimeState& state);
    void tickStatsUpdated(const IBtRuntime::TickStats& stats);
    void busMessageDispatched(const QString& topic, uint32_t sourceEntity, const QByteArray& payload);
    void logMessageReceived(const QString& level, const QString& message);
    void simulationStarted();
    void simulationPaused();
    void simulationStopped();

private:
    SimHostApp* app_ = nullptr;
    QThread* runtimeThread_ = nullptr;

    // 将运行时回调（来自 uvw/TBB 线程）转换为 Qt 信号
    void setupWakeupBridge();
    void setupBusInterceptor();
    void setupLogInterceptor();
};

}} // namespace
```

### 5.3 线程安全事件桥接

**问题**：TBB Worker 和 uvw Loop 是非 Qt 线程，不能直接发 Qt 信号。

**方案**：在 `SimHostAppFacade` 中注册原生 C++ 回调，回调内部通过 `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` 将事件投递到 Qt 事件循环。

```cpp
// 示例：拦截 CommandBus::Dispatch 事件
void SimHostAppFacade::setupBusInterceptor() {
    // 获取 InProcessCommandBus 实现（需暴露接口）
    auto* bus = static_cast<InProcessCommandBus*>(app_->CommandBusPtr().get());

    // 注册一个通用拦截 handler（在所有其他 handler 之后执行）
    bus->RegisterHandler("*", [this](const ActionCommand& cmd) {
        // 此 lambda 可能在 BT Tick 线程（主线程）或 uvw loop 线程执行
        // 安全地投递到 Qt 事件循环
        QMetaObject::invokeMethod(this, [this, cmd]() {
            emit busMessageDispatched(
                QString::fromStdString(cmd.command_type),
                cmd.source_entity,
                QByteArray(reinterpret_cast<const char*>(cmd.payload.data()),
                           static_cast<int>(cmd.payload.size()))
            );
        }, Qt::QueuedConnection);
    });
}
```

### 5.4 所需的后端最小接口扩展

为使前端能完整工作，后端需暴露以下原本为 internal 的能力：

| 前端需求 | 后端需新增/修改 | 文件 |
|---|---|---|
| 获取已注册节点类型列表 | `IBtRuntime::GetRegisteredNodeTypes()` 返回节点元信息 | `include/sim_bt/runtime/bt_runtime/i_bt_runtime.hpp` |
| 获取实体实时状态 | `IBtRuntime::GetTreeStatus(entity_id)` + `EntityContext` 读取 | `src/sim_host/sim_host_app.hpp` |
| 拦截所有 Bus 消息 | `InProcessCommandBus` 支持通配符 handler `"*"` 或添加 `SetGlobalInterceptor()` | `src/adapters/command_bus_impl.cpp` |
| 动态 Spawn/Despawn | 已有 `SpawnEntity` / `DespawnEntity`，需确保线程安全 | `src/sim_host/sim_host_app.cpp` |
| 暂停/单步 Tick | `SimHostApp` 新增 `Pause()` / `StepOne()` 方法 | `src/sim_host/sim_host_app.hpp` |
| 获取 TBB pending 数 | `TbbJobExecutor` 暴露 `PendingCount(priority)` | `src/runtime/compute_runtime/tbb_job_executor.hpp` |

---

## 6. 交互流程设计

### 6.1 创建一棵新行为树的完整流程

```
用户点击菜单 "文件 → 新建行为树"
    │
    ▼
QInputDialog 输入 Tree ID（如 "DroneReconTree"）
    │
    ▼
前端创建 BehaviorTreeModel，加入 ProjectModel.trees
    │
    ▼
切换到 TreeEditorWidget（QStackedWidget 切页）
    │
    ▼
用户从左侧 NodeLibraryDock 拖拽 "Fallback" 到画布
    │
    ▼
QGraphicsScene::dropEvent → 创建 BTNodeItem(type="Fallback")
    │
    ▼
继续拖拽 "Sequence" → "BatteryLevelCondition" → "ScanZoneAction"
    │
    ▼
右键 "连接" 或自动吸附连线（QGraphicsLineItem）
    │
    ▼
点击 "ScanZoneAction" → 右侧 PropertyDock 显示表单
    │
    ▼
用户修改 QComboBox priority=kNormal, QSpinBox timeout=5000
    │
    ▼
在 BlackboardDock 添加变量 "current_zone"(string), "threat_detected"(bool)
    │
    ▼
在 PropertyDock 的 PortTable 中将 ScanZoneAction 端口绑定到黑板变量
    │
    ▼
点击 "校验" 按钮 → 遍历模型检查必填端口 → QMessageBox::information("通过")
    │
    ▼
点击 "导出 XML" → 调用 BehaviorTreeModel::toXml() → QFileDialog 保存
    │
    ▼
切换到 EntityOrchestratorWidget → QPushButton "新增实体" → 绑定 "DroneReconTree"
    │
    ▼
点击主工具栏 "运行" → SimHostAppFacade::startSimulation()
```

### 6.2 配置 UDP Bus 出站的完整流程

```
用户点击菜单 "视图 → 总线配置中心"
    │
    ▼
切换到 BusConfigWidget（QStackedWidget 切页）
    │
    ▼
点击 "新建出站适配器"
    │
    ▼
填写 QFormLayout：
  name="CommandCenterUdp", protocol=UDP,
  remote_host="192.168.1.100", remote_port=8888,
  enabled_topics 勾选 threat_report 和 engage_target
    │
    ▼
点击 "测试连接"
    │
    ▼
Facade 临时创建 UvwUdpBusAdapter → Connect → Publish(探测消息)
    │
    ▼
等待回环或超时 → QMessageBox 显示 "UDP 目标可达" 或 "连接超时"
    │
    ▼
点击 "保存并启用"
    │
    ▼
Facade 调用 InProcessCommandBus::SetBusAdapter(adapter, event_loop)
    │
    ▼
适配器状态灯变绿，配置持久化到 ProjectModel
    │
    ▼
打开 MessageDebuggerDock，手动构造 ActionCommand，点击发送
    │
    ▼
MessageDebugger 的 QTableView 新增一行：
  [OUT] 14:32:01  threat_report  source=101  payload=0x65 00 ...
```

### 6.3 运行时调试流程

```
仿真运行中，EntityMonitorDock 的实体 103 卡片边框变红
    │
    ▼
用户点击卡片 → 展开详情显示 RUNNING 节点 = "ReportThreatAction"
    │
    ▼
双击卡片 → QStackedWidget 切到 TreeEditorWidget
    │
    ▼
TreeEditor 自动定位并高亮 ReportThreatAction 节点（蓝色闪烁）
    │
    ▼
用户右键该节点 → 上下文菜单 → "强制返回 SUCCESS"
    │
    ▼
Facade 调用 BtRuntimeImpl::HaltNode(entity_id, node_id) 并注入 SUCCESS
    │
    ▼
树继续执行，实体 103 卡片恢复正常，状态更新信号驱动 UI 刷新
```

---

## 7. 技术栈与项目结构

### 7.1 技术栈

| 层面 | 选择 | 理由 |
|---|---|---|
| **框架** | Qt 5.12+ (Widgets) | 与现有 C++ 项目无缝集成，不引入 JS 运行时 |
| **构建** | CMake（与主项目统一） | 同一构建系统，静态链接 sim-behavior 各库 |
| **画布** | `QGraphicsScene` / `QGraphicsView` | 原生 Qt，无需第三方库，节点编辑器完全可控 |
| **表格/列表** | `QTreeView` / `QTableView` + `QAbstractItemModel` | Model/View 架构，数据与表现分离 |
| **属性编辑** | `QFormLayout` + 自定义 PropertyGrid Widget | 灵活，可按类型动态生成控件 |
| **图表绘制** | 自定义 `QWidget` + `QPainter` | Tick 耗时折线图、饼图、柱状图轻量实现 |
| **2D 态势** | `QGraphicsScene`（独立 Scene）| 俯视图、图标、连线、动画 |
| **配置持久化** | `QSettings` + 自定义 JSON | 窗口布局用 `QSettings`，项目数据用 JSON |
| **多线程** | `QThread` + Qt 信号槽 | 运行时线程与 UI 线程分离，安全通信 |

### 7.2 建议的目录结构

```
sim-behavior/
├── ...
├── tools/
│   └── editor/                      ← 新增：Qt 可视化编辑器
│       ├── CMakeLists.txt
│       ├── src/
│       │   ├── main.cpp
│       │   ├── main_window.hpp/.cpp            # QMainWindow + 菜单/工具栏/状态栏
│       │   ├── app_context.hpp/.cpp            # 全局应用上下文（ProjectModel + Facade）
│       │   │
│       │   ├── editors/                        # 核心编辑器页面
│       │   │   ├── tree_editor_widget.hpp/.cpp       # QGraphicsView 行为树编辑器
│       │   │   ├── entity_orchestrator_widget.hpp/.cpp # 实体编排中心
│       │   │   ├── bus_config_widget.hpp/.cpp        # 总线配置中心
│       │   │   └── monitor_widget.hpp/.cpp           # 仿真监控台
│       │   │
│       │   ├── canvas/                         # QGraphics 图元
│       │   │   ├── bt_node_item.hpp/.cpp       # 节点图元
│       │   │   ├── bt_edge_item.hpp/.cpp       # 连线图元
│       │   │   ├── bt_port_item.hpp/.cpp       # 端口图元
│       │   │   └── world_entity_item.hpp/.cpp  # 2D 态势图元
│       │   │
│       │   ├── docks/                          # QDockWidget 面板
│       │   │   ├── node_library_dock.hpp/.cpp  # 节点库
│       │   │   ├── property_dock.hpp/.cpp      # 属性编辑器
│       │   │   ├── blackboard_dock.hpp/.cpp    # 黑板变量
│       │   │   ├── entity_monitor_dock.hpp/.cpp # Entity 状态监控
│       │   │   ├── message_debugger_dock.hpp/.cpp # Bus 消息调试
│       │   │   └── log_output_dock.hpp/.cpp    # 日志输出
│       │   │
│       │   ├── models/                         # C++ 数据模型
│       │   │   ├── project_model.hpp           # 核心结构体定义
│       │   │   ├── tree_xml_serializer.hpp/.cpp # XML ↔ Model 转换
│       │   │   └── project_json_serializer.hpp/.cpp # JSON ↔ Model 转换
│       │   │
│       │   ├── delegates/                      # Model/View 委托
│       │   │   ├── entity_card_delegate.hpp/.cpp
│       │   │   └── key_value_delegate.hpp/.cpp
│       │   │
│       │   ├── runtime/                        # 运行时桥接
│       │   │   └── sim_host_app_facade.hpp/.cpp # SimHostApp 封装 + 信号槽桥接
│       │   │
│       │   └── widgets/                        # 通用自定义控件
│       │       ├── property_grid.hpp/.cpp
│       │       ├── key_value_editor.hpp/.cpp
│       │       ├── vector3_edit.hpp/.cpp       # x,y,z 三输入框
│       │       └── status_indicator.hpp/.cpp   # 圆形状态灯
│       │
│       └── resources/
│           ├── icons/                            # .png/.svg 图标
│           ├── styles/                           # QSS 样式表
│           └── editor.qrc                        # Qt 资源文件
```

### 7.3 CMake 集成

```cmake
# tools/CMakeLists.txt
if(SIMBEHAVIOR_BUILD_EDITOR)
    find_package(Qt5 REQUIRED COMPONENTS Core Widgets)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)

    add_executable(sim_behavior_editor
        editor/src/main.cpp
        editor/src/main_window.cpp
        # ... 所有 .cpp 文件
        editor/resources/editor.qrc
    )

    target_link_libraries(sim_behavior_editor
        PRIVATE
            Qt5::Core
            Qt5::Widgets
            simbehavior_sim_host      # 直接链接运行时
            simbehavior_bt_runtime
            simbehavior_adapters
            simbehavior_domain
            # ... 其他必要库
    )
endif()
```

---

## 8. 关键 Qt Widgets 设计模式

### 8.1 MainWindow 的 Dock 布局恢复

```cpp
void MainWindow::saveLayout() {
    QSettings settings("sim-behavior", "editor");
    settings.setValue("geometry", saveGeometry());
    settings.setValue("windowState", saveState());
}

void MainWindow::restoreLayout() {
    QSettings settings("sim-behavior", "editor");
    restoreGeometry(settings.value("geometry").toByteArray());
    restoreState(settings.value("windowState").toByteArray());
}
```

### 8.2 QGraphicsItem 的状态同步

```cpp
class BTNodeItem : public QGraphicsItem {
public:
    void setRunning(bool running) {
        isRunning_ = running;
        update();  // 触发重绘
    }
protected:
    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        // 根据 isRunning_ 绘制不同边框色：RUNNING=蓝色, SUCCESS=绿色, FAILURE=红色, IDLE=灰色
        painter->setPen(QPen(isRunning_ ? Qt::blue : Qt::gray, 2));
        painter->drawRect(boundingRect());
        // ... 绘制文本、图标
    }
private:
    bool isRunning_ = false;
};
```

### 8.3 跨线程状态更新（避免直接操作 QGraphicsItem 于非 UI 线程）

```cpp
// 错误：从 TBB/运行时线程直接调用
entityItem->setRunning(true);  // ❌ 可能崩溃

// 正确：通过 Queued Connection 投递到 UI 线程
connect(facade_, &SimHostAppFacade::entityStateUpdated,
        this, [this](uint32_t eid, const EntityRuntimeState& state) {
            if (auto* item = findEntityItem(eid)) {
                item->setRunning(state.treeStatus == "running");
            }
        }, Qt::QueuedConnection);
```

---

## 9. 非功能需求

| 类别 | 要求 |
|---|---|
| **性能** | 画布支持 200+ 节点 60fps 拖拽；EntityMonitor 支持 100+ 实体 4fps 刷新（250ms 间隔） |
| **兼容性** | 生成的 XML 100% 兼容 BehaviorTree.CPP v4；JSON 100% 兼容 `config/simulation.json` |
| **可扩展** | 新增 C++ 节点类型后，前端通过重新编译或读取 JSON 元数据即可识别 |
| **离线可用** | 纯编辑功能无需启动仿真运行时 |
| **撤销重做** | 所有编辑操作支持 `QUndoStack`（Ctrl+Z / Ctrl+Y） |
| **自动保存** | 项目每 30 秒自动保存到临时文件，崩溃后可恢复 |
| **平台** | Windows (MSVC 2019+)、Linux (GCC 9+)，与 sim-behavior 主项目保持一致 |

---

## 10. 验收标准

- [ ] 行为树编辑器可 100% 还原 `examples/squad_patrol/trees/squad_tree.xml`
- [ ] 实体编排器可 100% 还原 `examples/squad_patrol/config/simulation.json`
- [ ] 导出的 XML 和 JSON 可被现有 C++ 示例直接加载运行
- [ ] 编译后的 `sim_behavior_editor` 可执行文件与 `squad_patrol_example` 链接同一套运行时库
- [ ] 运行时能在 Qt 界面中看到 5 个实体的状态变化、Tick 耗时曲线、Bus 消息流
- [ ] 在 Bus 配置中心新增 UDP 适配器后，`ReportThreatAction` 的 `threat_report` 可在 MessageDebugger 中截获显示
- [ ] 单步 Tick 模式下，每点击一次 "Step" 按钮，仿真推进一帧，EntityMonitor 刷新一次
