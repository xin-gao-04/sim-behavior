#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// sim_bt 进程级日志入口
//
// 使用方式：
//   1. 在进程启动时（SimHostApp::Initialize 内）调用 sim_bt::InitSimBtLog()
//   2. 在需要埋点的 .cpp 里 #include 本头文件，使用 SIMBT_LOG_* 宏
//   3. 进程退出时调用 sim_bt::ShutdownSimBtLog()（通常在析构中）
//
// logger 指针在 InitSimBtLog() 前为 nullptr；
// COREKIT_LOG_* 宏内部对 nullptr 做了保护（直接 return），
// 因此未初始化时调用宏是安全的 no-op。
// ─────────────────────────────────────────────────────────────────────────────

#include <string>

#include "corekit/log/log_macros.hpp"

namespace sim_bt {

// 返回进程级 logger 指针（未初始化时为 nullptr）。
// 直接传给 COREKIT_LOG_* 宏即可；宏对 nullptr 是安全 no-op。
corekit::log::ILogManager* SimBtLog();

// 初始化进程级 logger。
// 应在进程启动后尽早调用一次，多次调用幂等。
// app_name 用于日志文件命名及头部标识，默认 "sim_bt"。
void InitSimBtLog(const std::string& app_name = "sim_bt");

// 关闭 logger 并释放资源。
// 应在进程退出前调用，多次调用幂等。
void ShutdownSimBtLog();

}  // namespace sim_bt

// ─────────────────────────────────────────────────────────────────────────────
// 便捷宏：将 SimBtLog() 隐含为第一参数，避免每处显式传 logger
// ─────────────────────────────────────────────────────────────────────────────
#define SIMBT_LOG_INFO(msg)     COREKIT_LOG_INFO(::sim_bt::SimBtLog(), (msg))
#define SIMBT_LOG_WARN(msg)     COREKIT_LOG_WARNING(::sim_bt::SimBtLog(), (msg))
#define SIMBT_LOG_ERROR(msg)    COREKIT_LOG_ERROR(::sim_bt::SimBtLog(), (msg))

#define SIMBT_LOG_INFO_S(expr)  COREKIT_LOG_INFO_S(::sim_bt::SimBtLog(), expr)
#define SIMBT_LOG_WARN_S(expr)  COREKIT_LOG_WARNING_S(::sim_bt::SimBtLog(), expr)
#define SIMBT_LOG_ERROR_S(expr) COREKIT_LOG_ERROR_S(::sim_bt::SimBtLog(), expr)
