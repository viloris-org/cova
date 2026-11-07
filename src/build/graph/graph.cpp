// Cova — independent Build Graph IR.
#include "build/graph/graph.hpp"

#include <algorithm>
#include <queue>
#include <sstream>
#include <string>
#include <vector>

#include "core/filesystem/filesystem.hpp"

namespace cova::build {

ActionId BuildGraph::add(Action action) {
  ActionId id;
  id.value = actions_.empty() ? 1 : actions_.back().id.value + 1;
  action.id = id;
  index_[id.value] = actions_.size();
  actions_.push_back(std::move(action));
  return id;
}

const Action* BuildGraph::find(ActionId id) const {
  auto it = index_.find(id.value);
  return it == index_.end() ? nullptr : &actions_[it->second];
}

Action* BuildGraph::find(ActionId id) {
  auto it = index_.find(id.value);
  return it == index_.end() ? nullptr : &actions_[it->second];
}

std::vector<const Action*> BuildGraph::actions_in_order() const {
  std::map<std::uint64_t, std::size_t> pending;
  std::map<std::uint64_t, std::vector<std::uint64_t>> dependents;
  for (const auto& action : actions_) {
    pending[action.id.value] = 0;
  }
  for (const auto& action : actions_) {
    for (const auto& dependency : action.dependencies) {
      if (!index_.contains(dependency.value)) {
        continue;
      }
      pending[action.id.value] += 1;
      dependents[dependency.value].push_back(action.id.value);
    }
  }

  std::priority_queue<std::uint64_t, std::vector<std::uint64_t>, std::greater<>> ready;
  for (const auto& [id, count] : pending) {
    if (count == 0) {
      ready.push(id);
    }
  }

  std::vector<const Action*> ordered;
  ordered.reserve(actions_.size());
  while (!ready.empty()) {
    const std::uint64_t id = ready.top();
    ready.pop();
    const Action* action = find(ActionId{id});
    if (action != nullptr) {
      ordered.push_back(action);
    }
    for (const auto child : dependents[id]) {
      if (--pending[child] == 0) {
        ready.push(child);
      }
    }
  }
  // A cycle would leave actions out. Append them deterministically so the
  // executor can report the problem instead of silently dropping work.
  if (ordered.size() != actions_.size()) {
    for (const auto& action : actions_) {
      if (std::find(ordered.begin(), ordered.end(), &action) == ordered.end()) {
        ordered.push_back(&action);
      }
    }
  }
  return ordered;
}

std::string BuildGraph::to_text() const {
  std::ostringstream out;
  for (const auto* action : actions_in_order()) {
    out << "[" << action->id.value << "] " << to_string(action->kind) << " "
        << action->label;
    if (!action->package.empty()) {
      out << " (" << action->package << ")";
    }
    out << "\n";
    for (const auto& input : action->inputs) {
      out << "    in:  " << fs::generic_string(input.path) << "\n";
    }
    for (const auto& output : action->outputs) {
      out << "    out: " << fs::generic_string(output.path) << "\n";
    }
    for (const auto& dependency : action->dependencies) {
      out << "    dep: " << dependency.value << "\n";
    }
  }
  return out.str();
}

std::string BuildGraph::to_dot() const {
  std::ostringstream out;
  out << "digraph cova_build {\n";
  out << "  rankdir=LR;\n";
  for (const auto* action : actions_in_order()) {
    out << "  n" << action->id.value << " [label=\"" << action->label << "\"];\n";
    for (const auto& dependency : action->dependencies) {
      out << "  n" << dependency.value << " -> n" << action->id.value << ";\n";
    }
  }
  out << "}\n";
  return out.str();
}

}  // namespace cova::build
