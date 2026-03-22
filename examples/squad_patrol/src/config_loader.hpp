#pragma once
//
// config_loader.hpp — 从 JSON 文件加载仿真参数
//
// 依赖：BehaviorTree.CPP 内置的 nlohmann/json（contrib/json.hpp）
// 用法：
//   auto cfg = squad_example::LoadConfig("config/simulation.json");
//   std::cout << cfg.squad.members.size() << " entities\n";
//

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

// BehaviorTree.CPP 内置了 nlohmann/json，通过以下路径可用。
// 若需要独立项目，可改为 #include <nlohmann/json.hpp>（单独安装）。
#include <behaviortree_cpp/contrib/json.hpp>

#include "sim_bt/common/types.hpp"  // EntityId, GroupId

namespace squad_example {

// ─────────────────────────────────────────────────────────────────────────────
// 配置数据结构（与 simulation.json 对应）
// ─────────────────────────────────────────────────────────────────────────────

// 单个分队成员配置
struct MemberConfig {
    sim_bt::EntityId id   = 0;
    std::string      name;
    std::string      zone;   // 巡逻区域标识（north / south / east / west / center）
};

// 分队（GroupContext）配置
struct SquadConfig {
    sim_bt::GroupId           id   = 0;
    std::string               name;
    std::vector<MemberConfig> members;
    float obj_x   = 0, obj_y = 0, obj_z = 0;    // 任务目标位置
    float rally_x = 0, rally_y = 0, rally_z = 0; // 集结点
};

// 完整仿真配置
struct SimulationConfig {
    // 仿真总体参数
    std::string name;
    int         duration_frames      = 60;        // 总运行帧数
    std::string tick_policy;                      // "skip_idle" / "tick_all"

    // 行为参数
    std::string tree_file;                        // BT XML 文件路径（相对于可执行文件）
    int         scan_duration_ms     = 300;       // 扫描模拟时长（ms）
    int         patrol_step_ms       = 500;       // 移动模拟时长（ms）
    int         report_duration_ms   = 100;       // 上报确认时延（ms）
    int         threat_probability_pct = 25;      // 威胁概率 0~100

    // 分队数据
    SquadConfig squad;
};

// ─────────────────────────────────────────────────────────────────────────────
// LoadConfig — 从 JSON 文件读取并解析配置
//
// 参数：
//   path — JSON 文件路径（可以是绝对路径或相对于 CWD 的路径）
//
// 异常：
//   std::runtime_error — 文件不存在或 JSON 格式错误
// ─────────────────────────────────────────────────────────────────────────────
inline SimulationConfig LoadConfig(const std::string& path) {
    // ── 读取文件 ──────────────────────────────────────────────────────────────
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error(
            "[config_loader] 无法打开配置文件: " + path +
            "\n  提示：请确认 config/simulation.json 已复制到可执行文件目录");
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(
            std::string("[config_loader] JSON 解析失败: ") + e.what());
    }

    // ── 解析仿真总体参数 ──────────────────────────────────────────────────────
    SimulationConfig cfg;
    cfg.name            = j.at("simulation").at("name").get<std::string>();
    cfg.duration_frames = j.at("simulation").at("duration_frames").get<int>();
    cfg.tick_policy     = j.at("simulation").at("tick_policy").get<std::string>();

    // ── 解析行为参数 ─────────────────────────────────────────────────────────
    const auto& beh = j.at("behavior");
    cfg.tree_file               = beh.at("tree_file").get<std::string>();
    cfg.scan_duration_ms        = beh.at("scan_duration_ms").get<int>();
    cfg.patrol_step_ms          = beh.at("patrol_step_ms").get<int>();
    cfg.report_duration_ms      = beh.at("report_duration_ms").get<int>();
    cfg.threat_probability_pct  = beh.at("threat_probability_pct").get<int>();

    // ── 解析分队配置 ─────────────────────────────────────────────────────────
    const auto& sq = j.at("squad");
    cfg.squad.id   = sq.at("id").get<uint32_t>();
    cfg.squad.name = sq.at("name").get<std::string>();

    // 目标位置与集结点（可选字段，缺失时保留默认值 0）
    if (sq.contains("objective")) {
        cfg.squad.obj_x = sq["objective"].value("x", 0.0f);
        cfg.squad.obj_y = sq["objective"].value("y", 0.0f);
        cfg.squad.obj_z = sq["objective"].value("z", 0.0f);
    }
    if (sq.contains("rally_point")) {
        cfg.squad.rally_x = sq["rally_point"].value("x", 0.0f);
        cfg.squad.rally_y = sq["rally_point"].value("y", 0.0f);
        cfg.squad.rally_z = sq["rally_point"].value("z", 0.0f);
    }

    // 成员列表
    for (const auto& m : sq.at("members")) {
        MemberConfig mc;
        mc.id   = m.at("id").get<uint32_t>();
        mc.name = m.at("name").get<std::string>();
        mc.zone = m.at("zone").get<std::string>();
        cfg.squad.members.push_back(std::move(mc));
    }

    return cfg;
}

}  // namespace squad_example
