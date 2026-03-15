#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// NodeStatus
//
// 行为树节点执行状态，与 BehaviorTree.CPP 的 NodeStatus 对齐。
// ─────────────────────────────────────────────────────────────────────────────
enum class NodeStatus : uint8_t {
  kIdle    = 0,  // 未启动
  kRunning = 1,  // 正在执行（异步动作返回此状态等待下次 tick）
  kSuccess = 2,  // 执行成功
  kFailure = 3,  // 执行失败
};

// ─────────────────────────────────────────────────────────────────────────────
// JobPriority
//
// oneTBB task_arena 任务优先级，对应三类资源池：
//   kHigh   — 战术决策、威胁评估、火力分配
//   kNormal — 路径规划、几何计算、地形查询
//   kLow    — 预取、后台统计、离线缓存
// ─────────────────────────────────────────────────────────────────────────────
enum class JobPriority : uint8_t {
  kHigh   = 0,
  kNormal = 1,
  kLow    = 2,
};

// ─────────────────────────────────────────────────────────────────────────────
// EntityId / GroupId
//
// 仿真实体与编队的标识符，使用 uint32_t 以匹配典型仿真系统约定。
// ─────────────────────────────────────────────────────────────────────────────
using EntityId = uint32_t;
using GroupId  = uint32_t;

constexpr EntityId kInvalidEntityId = 0;
constexpr GroupId  kInvalidGroupId  = 0;

// ─────────────────────────────────────────────────────────────────────────────
// SimTime
//
// 仿真时间戳，单位毫秒（整数），避免浮点累积误差。
// ─────────────────────────────────────────────────────────────────────────────
using SimTimeMs = uint64_t;

// ─────────────────────────────────────────────────────────────────────────────
// 通用回调类型
// ─────────────────────────────────────────────────────────────────────────────
using VoidCallback  = std::function<void()>;
using ErrorCallback = std::function<void(const std::string& reason)>;

}  // namespace sim_bt
