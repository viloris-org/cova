// Cova — pure dependency scheduler (PRD §9.4).
//
// The scheduler knows nothing about processes or compilers: it tracks which
// actions are ready, running and complete.
#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <vector>

#include "build/graph/graph.hpp"

namespace cova::build {

class Scheduler {
 public:
  explicit Scheduler(const BuildGraph& graph);

  // Marks an action complete and releases its dependents.
  void complete(ActionId id);
  // Marks an action failed. Its dependents are never released.
  void fail(ActionId id);

  // Next ready action, or nullopt when nothing can run right now.
  std::optional<ActionId> next();

  bool done() const;
  bool failed() const { return failed_; }
  std::size_t remaining() const;

  // Number of actions that are ready but not yet handed out.
  std::size_t ready_count() const { return ready_.size(); }

 private:
  struct Node {
    const Action* action = nullptr;
    std::size_t pending_dependencies = 0;
    std::size_t dependents = 0;
    bool scheduled = false;
    bool finished = false;
  };
  std::map<std::uint64_t, Node> nodes_;
  std::vector<ActionId> ready_;
  bool failed_ = false;
};

}  // namespace cova::build
