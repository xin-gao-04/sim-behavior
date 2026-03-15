#pragma once

#include <string>
#include <utility>

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// SimStatus / SimResult
//
// 轻量错误传播类型，与 corekit::api::Status / Result 语义对齐，
// 但不依赖 corekit 头文件，保持行为树模块独立可测试。
//
// 如果需要在边界上与 corekit 互转，使用适配器层完成转换。
// ─────────────────────────────────────────────────────────────────────────────

struct SimStatus {
  bool        ok      = true;
  int         code    = 0;
  std::string message;

  static SimStatus Ok() { return {true, 0, {}}; }

  static SimStatus Err(int code, std::string msg) {
    return {false, code, std::move(msg)};
  }

  explicit operator bool() const { return ok; }
};

template <typename T>
struct SimResult {
  SimStatus status;
  T         value{};

  SimResult() = default;

  explicit SimResult(T val) : status(SimStatus::Ok()), value(std::move(val)) {}

  explicit SimResult(SimStatus s) : status(std::move(s)) {}

  bool ok() const { return status.ok; }
};

}  // namespace sim_bt
