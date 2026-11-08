// Cova — pure dependency scheduler.
#include "build/scheduler/scheduler.hpp"

#include <algorithm>
#include <vector>

namespace cova::build {

Scheduler::Scheduler(const BuildGraph& graph) {
  for (const auto& action : graph.actions()) {
    Node node;
    node.action = &action;
    for (const auto& dependency : action.dependencies) {
      if (nodes_.contains(dependency.value)) {
        node.pending_dependencies += 1;
        nodes_[dependency.value].dependents += 1;
      }
    }
    nodes_[action.id.value] = node;
  }
  for (auto& [id, node] : nodes_) {
    if (node.pending_dependencies == 0) {
      ready_.push_back(ActionId{id});
    }
  }
}

void Scheduler::complete(ActionId id) {
  auto it = nodes_.find(id.value);
  if (it == nodes_.end() || it->second.finished) {
    return;
  }
  it->second.finished = true;
  for (auto& [other_id, node] : nodes_) {
    if (node.finished) {
      continue;
    }
    const auto& dependencies = node.action->dependencies;
    const bool depends_on_completed =
        std::find(dependencies.begin(), dependencies.end(), id) != dependencies.end();
    if (!depends_on_completed) {
      continue;
    }
    if (node.pending_dependencies > 0) {
      node.pending_dependencies -= 1;
    }
    if (node.pending_dependencies == 0 && !node.scheduled) {
      node.scheduled = true;
      ready_.push_back(ActionId{other_id});
    }
  }
}

void Scheduler::fail(ActionId id) {
  auto it = nodes_.find(id.value);
  if (it != nodes_.end()) {
    it->second.finished = true;
  }
  failed_ = true;
}

std::optional<ActionId> Scheduler::next() {
  if (ready_.empty()) {
    return std::nullopt;
  }
  ActionId id = ready_.front();
  ready_.erase(ready_.begin());
  auto it = nodes_.find(id.value);
  if (it != nodes_.end()) {
    it->second.scheduled = true;
  }
  return id;
}

bool Scheduler::done() const {
  for (const auto& [id, node] : nodes_) {
    (void)id;
    if (!node.finished) {
      return false;
    }
  }
  return true;
}

std::size_t Scheduler::remaining() const {
  std::size_t count = 0;
  for (const auto& [id, node] : nodes_) {
    (void)id;
    if (!node.finished) {
      ++count;
    }
  }
  return count;
}

}  // namespace cova::build
