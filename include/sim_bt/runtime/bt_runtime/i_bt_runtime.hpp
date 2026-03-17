#pragma once

#include <functional>
#include <memory>
#include <string>

#include <behaviortree_cpp/bt_factory.h>

#include "sim_bt/bt_nodes/i_async_action_context.hpp"
#include "sim_bt/bt_nodes/i_sync_node_context.hpp"
#include "sim_bt/common/result.hpp"
#include "sim_bt/common/types.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// ITreeInstance
//
// 单棵行为树实例的运行时句柄。
// 对 BehaviorTree.CPP 的 BT::Tree 的封装。
// ─────────────────────────────────────────────────────────────────────────────
class ITreeInstance {
 public:
  virtual ~ITreeInstance() = default;

  virtual EntityId    OwnerEntity() const = 0;
  virtual const char* TreeName() const = 0;

  // 执行一次 tick，返回根节点状态。
  // 必须在 BT Tick Domain（单线程）中调用。
  virtual NodeStatus Tick() = 0;

  // 向树发出 halt 信号（BehaviorTree.CPP haltTree()）。
  // 使所有 RUNNING 节点调用 halt()，线程安全（通过内部标志延迟到 tick 前执行）。
  virtual void Halt() = 0;

  // 重置树到初始状态。
  virtual void Reset() = 0;

  // 是否有任何节点处于 RUNNING 状态。
  virtual bool HasRunningNodes() const = 0;
};

using TreeInstancePtr = std::shared_ptr<ITreeInstance>;

// ─────────────────────────────────────────────────────────────────────────────
// IBtRuntime
//
// 行为树运行时（Behavior Runtime 第二层）。
//
// 职责：
//   - 封装 BehaviorTree.CPP 的 BT::BehaviorTreeFactory 和节点注册。
//   - 从 XML 或内存描述符创建 ITreeInstance。
//   - 管理多实体的树实例生命周期。
//   - 驱动 TickScheduler：每帧对活跃实体逐个 tick。
//   - 提供 WakeUp 接口：供 EventLoopRuntime 通知特定树需要立即 re-tick。
//
// BT 节点注册：
//   各模块在 IBtRuntime::RegisterNodes() 等方式中注册自定义节点类型，
//   注册完成后调用 LoadTree() 才能实例化对应节点。
// ─────────────────────────────────────────────────────────────────────────────
class IBtRuntime {
 public:
  virtual ~IBtRuntime() = default;

  // ── 生命周期 ────────────────────────────────────────────────────────────────

  virtual SimStatus Initialize() = 0;
  virtual void Shutdown() = 0;

  // ── 节点注册 ────────────────────────────────────────────────────────────────

  // 注册一个节点类型（通过 manifest + BT::NodeBuilder 闭包）。
  // 须在 LoadTreeFromXml/File 之前调用，否则 XML 解析时找不到节点定义。
  //
  // 直接使用 BT::TreeNodeManifest 版本，避免在虚函数中引入模板类型参数。
  // 多实体场景推荐：builder 中无需捕获 ctx，节点从 Blackboard 自动读取。
  virtual void RegisterNodeBuilder(const BT::TreeNodeManifest& manifest,
                                   BT::NodeBuilder             builder) = 0;

  // 便捷模板包装：从 NodeT::providedPorts() 自动构建 manifest，
  // 创建以 (name, config) 构造节点的默认 builder。
  // 节点 ctx 参数须有默认值 nullptr（从 Blackboard 读取）。
  template <typename NodeT>
  void RegisterNodeType(const std::string& name) {
    auto manifest = BT::CreateManifest<NodeT>(name, NodeT::providedPorts());
    RegisterNodeBuilder(manifest,
        [](const std::string& n, const BT::NodeConfig& c) {
          return std::make_unique<NodeT>(n, c);
        });
  }

  // ── 树工厂 ──────────────────────────────────────────────────────────────────

  // 从 XML 字符串加载树定义（注册到工厂但不实例化）。
  virtual SimStatus LoadTreeFromXml(const std::string& xml) = 0;

  // 从文件路径加载树定义。
  virtual SimStatus LoadTreeFromFile(const std::string& path) = 0;

  // ── 实例管理 ────────────────────────────────────────────────────────────────

  // 为实体创建指定名称的树实例。
  //
  // async_ctx / sync_ctx：可选的 per-entity 上下文；
  //   若提供，会注入到树的私有 Blackboard（"__async_ctx__" / "__sync_ctx__"），
  //   节点在构造时自动从 Blackboard 读取（无需在 registerBuilder 捕获 ctx）。
  //   若不提供，节点须通过 registerBuilder 闭包显式注入 ctx。
  virtual SimResult<TreeInstancePtr> CreateTree(
      EntityId entity_id,
      const std::string& tree_name,
      AsyncActionContextPtr async_ctx = nullptr,
      SyncNodeContextPtr    sync_ctx  = nullptr) = 0;

  // 销毁实体的树实例（若有 RUNNING 节点会先 Halt）。
  virtual void DestroyTree(EntityId entity_id) = 0;

  // 获取实体的当前树实例，不存在时返回 nullptr。
  virtual TreeInstancePtr GetTree(EntityId entity_id) const = 0;

  // ── Tick 调度 ───────────────────────────────────────────────────────────────

  // 对所有活跃实体逐个 tick，在 BT Tick Domain 中调用。
  // sim_time_ms 为当前仿真帧时间。
  virtual void TickAll(SimTimeMs sim_time_ms) = 0;

  // 对指定实体立即 tick（通常由 WakeupBridge 触发）。
  virtual void TickEntity(EntityId entity_id) = 0;

  // ── 唤醒 ────────────────────────────────────────────────────────────────────

  // 从任意线程通知特定实体需要 re-tick（线程安全，投递到 tick 前队列）。
  virtual void RequestWakeup(EntityId entity_id) = 0;

  // ── 统计 ────────────────────────────────────────────────────────────────────

  virtual size_t ActiveTreeCount() const = 0;
};

using BtRuntimePtr = std::shared_ptr<IBtRuntime>;

}  // namespace sim_bt
