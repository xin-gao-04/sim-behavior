#include "sim_bt/domain/group/i_group_context.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace sim_bt {

class GroupContextImpl : public IGroupContext {
 public:
  explicit GroupContextImpl(GroupId id) : id_(id) {}

  GroupId Id() const override { return id_; }

  const std::vector<EntityId>& Members() const override { return members_; }

  bool IsMember(EntityId id) const override {
    return std::find(members_.begin(), members_.end(), id) != members_.end();
  }

  void AddMember(EntityId id) {
    if (!IsMember(id)) members_.push_back(id);
  }

  void SetObjectivePosition(float x, float y, float z) override {
    obj_x_ = x; obj_y_ = y; obj_z_ = z;
  }
  void GetObjectivePosition(float& x, float& y, float& z) const override {
    x = obj_x_; y = obj_y_; z = obj_z_;
  }

  void SetRallyPoint(float x, float y, float z) override {
    rally_x_ = x; rally_y_ = y; rally_z_ = z;
  }
  void GetRallyPoint(float& x, float& y, float& z) const override {
    x = rally_x_; y = rally_y_; z = rally_z_;
  }

  void SetRule(const std::string& key, bool value) override { rules_[key] = value; }
  bool GetRule(const std::string& key, bool default_value) const override {
    auto it = rules_.find(key);
    return (it != rules_.end()) ? it->second : default_value;
  }

  void SetLastPlanJobId(uint64_t job_id) override { last_plan_job_id_ = job_id; }
  uint64_t LastPlanJobId() const override { return last_plan_job_id_; }

 private:
  GroupId id_;
  std::vector<EntityId> members_;

  float obj_x_ = 0, obj_y_ = 0, obj_z_ = 0;
  float rally_x_ = 0, rally_y_ = 0, rally_z_ = 0;

  std::unordered_map<std::string, bool> rules_;
  uint64_t last_plan_job_id_ = 0;
};

}  // namespace sim_bt
