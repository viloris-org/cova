// Cova — independent Build Graph IR (PRD §9.1).
//
// Action order is explicit. Compiler policy lives in the planner, not here.
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "build/action/action.hpp"

namespace cova::build {

class BuildGraph {
 public:
  // Adds an action, assigning a fresh id. Dependencies must refer to already
  // added actions.
  ActionId add(Action action);

  const Action* find(ActionId id) const;
  Action* find(ActionId id);

  std::size_t size() const { return actions_.size(); }
  bool empty() const { return actions_.empty(); }

  // All actions in dependency order. The plan is already ordered, but this
  // method verifies and repairs the order for callers that add out of order.
  std::vector<const Action*> actions_in_order() const;

  // Human and Graphviz renderings for `cova graph` (PRD §4.1).
  std::string to_text() const;
  std::string to_dot() const;

  const std::vector<Action>& actions() const { return actions_; }
  std::vector<Action>& actions() { return actions_; }

 private:
  std::vector<Action> actions_;
  std::map<std::uint64_t, std::size_t> index_;
};

}  // namespace cova::build
