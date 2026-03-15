#pragma once

#include <string>
#include <unordered_map>

#include "sim_bt/domain/entity/i_entity_context.hpp"

namespace sim_bt {

class EntityContextImpl : public IEntityContext {
 public:
  explicit EntityContextImpl(EntityId id) : id_(id) {}
  ~EntityContextImpl() override = default;

  EntityId Id() const override { return id_; }

  void SetCurrentTarget(EntityId target_id) override { current_target_ = target_id; }
  EntityId CurrentTarget() const override { return current_target_; }

  void SetFlag(const std::string& key, bool value) override { flags_[key] = value; }
  bool GetFlag(const std::string& key, bool default_value) const override {
    auto it = flags_.find(key);
    return (it != flags_.end()) ? it->second : default_value;
  }

  void SetInt(const std::string& key, int64_t value) override { ints_[key] = value; }
  int64_t GetInt(const std::string& key, int64_t default_value) const override {
    auto it = ints_.find(key);
    return (it != ints_.end()) ? it->second : default_value;
  }

  void SetFloat(const std::string& key, double value) override { floats_[key] = value; }
  double GetFloat(const std::string& key, double default_value) const override {
    auto it = floats_.find(key);
    return (it != floats_.end()) ? it->second : default_value;
  }

  SimTimeMs LastTickTime() const override { return last_tick_time_; }
  void UpdateLastTickTime(SimTimeMs t) override { last_tick_time_ = t; }

 private:
  EntityId   id_;
  EntityId   current_target_ = kInvalidEntityId;
  SimTimeMs  last_tick_time_ = 0;

  std::unordered_map<std::string, bool>    flags_;
  std::unordered_map<std::string, int64_t> ints_;
  std::unordered_map<std::string, double>  floats_;
};

}  // namespace sim_bt
