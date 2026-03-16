#include "sim_bt/common/sim_bt_log.hpp"

#include "corekit/api/factory.hpp"

namespace sim_bt {

// 进程级 logger 实例（只读指针，生命周期由 Init/Shutdown 管理）
static corekit::log::ILogManager* g_logger = nullptr;

corekit::log::ILogManager* SimBtLog() {
  return g_logger;
}

void InitSimBtLog(const std::string& app_name) {
  if (g_logger) return;  // 幂等
  g_logger = corekit_create_log_manager();
  if (g_logger) {
    g_logger->Init(app_name, /*config_path=*/"");
  }
}

void ShutdownSimBtLog() {
  if (!g_logger) return;  // 幂等
  g_logger->Shutdown();
  corekit_destroy_log_manager(g_logger);
  g_logger = nullptr;
}

}  // namespace sim_bt
