// Cova — explicit build environment policy tests (FR-01).
#include <algorithm>
#include <map>
#include <string>

#include "core/process/environment.hpp"
#include "testing/test.hpp"

namespace {

const cova::proc::EnvironmentEntry* find_entry(
    const cova::proc::EnvironmentSnapshot& snapshot, std::string_view name) {
  for (const auto& entry : snapshot.entries) {
    if (entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

bool contains_name(const std::vector<std::string>& names, std::string_view name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

}  // namespace

COVA_TEST_NAMED(core, environment_default_forwards_only_audited_variables) {
  cova::test::ScopedEnv path("PATH", "/usr/bin:/bin");
  cova::test::ScopedEnv cpath("CPATH", "/opt/cova-one");
  cova::test::ScopedEnv cflags("CFLAGS", "-O3");
  cova::test::ScopedEnv unknown("COVA_ENV_LEAK", "leaked");
  cova::test::ScopedEnv secret("COVA_API_TOKEN", "top-secret-value");

  auto snapshot = COVA_REQUIRE_OK(cova::proc::collect_environment());
  COVA_CHECK(snapshot.isolation == cova::proc::EnvironmentIsolation::Hermetic);
  COVA_CHECK(snapshot.shared_cache_eligible);
  COVA_CHECK(snapshot.incremental_eligible);

  const auto* cpath_entry = find_entry(snapshot, "CPATH");
  COVA_CHECK(cpath_entry != nullptr);
  COVA_CHECK_EQ(cpath_entry->value, std::string("/opt/cova-one"));
  COVA_CHECK(cpath_entry->tracked);
  COVA_CHECK(find_entry(snapshot, "PATH") != nullptr);
  COVA_CHECK(find_entry(snapshot, "COVA_ENV_LEAK") == nullptr);
  COVA_CHECK(find_entry(snapshot, "CFLAGS") == nullptr);
  COVA_CHECK(find_entry(snapshot, "COVA_API_TOKEN") == nullptr);
  COVA_CHECK(contains_name(snapshot.rejected, "CFLAGS"));
  COVA_CHECK(contains_name(snapshot.rejected, "COVA_ENV_LEAK"));
  COVA_CHECK(contains_name(snapshot.rejected, "COVA_API_TOKEN"));
  // Secrets never reach the identity.
  COVA_CHECK(snapshot.identity().find("top-secret-value") == std::string::npos);
}

COVA_TEST_NAMED(core, environment_identity_changes_with_tracked_values) {
  std::string first_identity;
  {
    cova::test::ScopedEnv cpath("CPATH", "/opt/cova-one");
    first_identity = COVA_REQUIRE_OK(cova::proc::collect_environment()).identity();
  }
  {
    cova::test::ScopedEnv cpath("CPATH", "/opt/cova-two");
    const std::string second_identity =
        COVA_REQUIRE_OK(cova::proc::collect_environment()).identity();
    COVA_CHECK(first_identity != second_identity);
  }

  cova::proc::EnvironmentRequest request;
  request.additions.push_back(cova::proc::EnvironmentVariable{"COVA_EXTRA", "one", true});
  auto tracked_one = COVA_REQUIRE_OK(cova::proc::collect_environment(request));
  request.additions[0].value = "two";
  auto tracked_two = COVA_REQUIRE_OK(cova::proc::collect_environment(request));
  COVA_CHECK(tracked_one.identity() != tracked_two.identity());
}

COVA_TEST_NAMED(core, environment_inherit_untracked_downgrades_reuse) {
  cova::test::ScopedEnv leak("COVA_ENV_LEAK", "leaked-value");
  cova::test::ScopedEnv flags("CFLAGS", "-O3");
  cova::proc::EnvironmentRequest request;
  request.inherit_untracked = true;
  auto snapshot = COVA_REQUIRE_OK(cova::proc::collect_environment(request));
  COVA_CHECK(snapshot.isolation == cova::proc::EnvironmentIsolation::UntrackedEnvironment);
  COVA_CHECK(!snapshot.shared_cache_eligible);
  COVA_CHECK(!snapshot.incremental_eligible);
  const auto* entry = find_entry(snapshot, "COVA_ENV_LEAK");
  COVA_CHECK(entry != nullptr);
  COVA_CHECK(!entry->tracked);
  // Inheriting untracked variables must not bypass an explicit rejection.
  COVA_CHECK(find_entry(snapshot, "CFLAGS") == nullptr);
  COVA_CHECK(contains_name(snapshot.rejected, "CFLAGS"));
  // An untracked value is not allowed to become part of a cache identity.
  COVA_CHECK(snapshot.identity().find("leaked-value") == std::string::npos);
}

COVA_TEST_NAMED(core, environment_rejects_flag_bags_and_secrets) {
  cova::proc::EnvironmentRequest flags;
  flags.additions.push_back(cova::proc::EnvironmentVariable{"CFLAGS", "-O3", true});
  auto rejected = cova::proc::collect_environment(flags);
  COVA_CHECK(!rejected.has_value());
  COVA_CHECK_EQ(rejected.error().code, std::string("cova::proc::environment_rejected"));

  cova::proc::EnvironmentRequest secret;
  secret.additions.push_back(cova::proc::EnvironmentVariable{"SERVICE_PASSWORD", "hunter2", true});
  auto refused = cova::proc::collect_environment(secret);
  COVA_CHECK(!refused.has_value());
  COVA_CHECK_EQ(refused.error().code, std::string("cova::proc::environment_secret"));
}

COVA_TEST_NAMED(core, environment_locale_prefix_is_tracked) {
  COVA_CHECK(cova::proc::is_tracked_environment_variable("LC_MESSAGES"));
  COVA_CHECK(cova::proc::is_tracked_environment_variable("LC_WHATEVER"));
  COVA_CHECK(cova::proc::environment_policy("LC_WHATEVER") ==
             cova::proc::EnvironmentPolicy::InheritedTracked);
  COVA_CHECK(!cova::proc::is_tracked_environment_variable("COVA_LEAK"));
}

COVA_TEST_NAMED(core, environment_removals_are_never_forwarded) {
  cova::test::ScopedEnv cpath("CPATH", "/opt/cova-one");
  cova::proc::EnvironmentRequest request;
  request.removals.push_back("CPATH");
  auto snapshot = COVA_REQUIRE_OK(cova::proc::collect_environment(request));
  COVA_CHECK(find_entry(snapshot, "CPATH") == nullptr);
  COVA_CHECK(contains_name(snapshot.rejected, "CPATH"));
}

COVA_TEST_NAMED(core, process_environment_reads_the_host) {
  const auto environment = cova::proc::process_environment();
  COVA_CHECK(!environment.empty());
  const bool sorted = std::is_sorted(environment.begin(), environment.end(),
                                     [](const auto& left, const auto& right) {
                                       return left.first < right.first;
                                     });
  COVA_CHECK(sorted);
}

COVA_TEST_MAIN()
