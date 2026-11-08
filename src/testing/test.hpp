// Cova — minimal in-tree test framework.
//
// Cova must bootstrap without third-party dependencies (PRD §16.3), so its test
// framework is a few hundred lines rather than GoogleTest.
//
// Usage:
//   COVA_TEST(case_name) { COVA_CHECK(1 + 1 == 2); COVA_CHECK_EQ(a, b); }
//   int main() { return cova::test::run_all(); }
#pragma once

#include <chrono>
#include <functional>
#include <iosfwd>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace cova::test {

// One registered test case.
struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> body;
  std::string file;
  int line = 0;
};

// Process-wide registry (function-local static to avoid init-order fiasco).
std::vector<TestCase>& registry();

struct Registration {
  Registration(std::string suite, std::string name, std::function<void()> body,
               std::string file, int line);
};

// Thrown by a failed assertion; caught by the runner.
struct AssertionFailure {
  std::string message;
  std::string file;
  int line = 0;
  std::string expression;
};

[[noreturn]] void fail_assertion(std::string message, std::string file, int line,
                                 std::string expression);

struct Options {
  // Substring filter on "suite.name".
  std::string filter;
  // Milliseconds; 0 disables.
  int timeout_ms = 0;
  bool list_only = false;
  bool verbose = false;
  // Emit one line per test instead of a compact summary.
  bool tap = false;
  bool no_color = false;
};

struct CaseResult {
  std::string full_name;
  bool passed = true;
  bool skipped = false;
  std::string message;
  std::chrono::milliseconds duration{0};
};

struct Suite {
  std::vector<CaseResult> results;

  std::size_t passed() const;
  std::size_t failed() const;
  std::size_t skipped() const;
  bool ok() const { return failed() == 0; }
};

Suite run_all(const Options& options = {});
Suite run_all(int argc, char** argv);

// Helpers used by the macros.
std::string format_value(const std::string& value);
std::string format_value(const char* value);
std::string format_value(std::string_view value);
std::string format_value(bool value);
template <class T>
std::string format_value(const T& value) {
  std::ostringstream os;
  os << value;
  return os.str();
}

// Temporary directory that is removed on destruction. Tests never touch the
// user's real ~/.cova.
class TempDir {
 public:
  TempDir();
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) noexcept;
  TempDir& operator=(TempDir&&) noexcept;

  const std::string& path() const { return path_; }
  std::string sub(std::string_view name) const;

 private:
  // Removes the tree; noexcept so the destructor never throws (additive).
  void cleanup() noexcept;

  std::string path_;
};

// Sets an environment variable for the lifetime of this object.
class ScopedEnv {
 public:
  ScopedEnv(std::string name, std::string value);
  ~ScopedEnv();
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  std::string name_;
  std::string old_;
  bool had_old_ = false;
};

}  // namespace cova::test

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------
#define COVA_TEST_CASE_(suite_name, test_name)                                        \
  static void cova_test_body_##suite_name##_##test_name();                            \
  static ::cova::test::Registration cova_test_reg_##suite_name##_##test_name(         \
      #suite_name, #test_name, &cova_test_body_##suite_name##_##test_name, __FILE__,  \
      __LINE__);                                                                      \
  static void cova_test_body_##suite_name##_##test_name()

#define COVA_TEST(test_name) COVA_TEST_CASE_(cova, test_name)

#define COVA_TEST_NAMED(suite_name, test_name) COVA_TEST_CASE_(suite_name, test_name)

#define COVA_FAIL_AT(msg, expr) \
  ::cova::test::fail_assertion((msg), __FILE__, __LINE__, (expr))

#define COVA_CHECK(expr)                                                          \
  do {                                                                            \
    if (!(expr)) {                                                                \
      COVA_FAIL_AT("expected: " #expr, #expr);                                    \
    }                                                                             \
  } while (false)

#define COVA_CHECK_MSG(expr, msg)                                                 \
  do {                                                                            \
    if (!(expr)) {                                                                \
      COVA_FAIL_AT((msg), #expr);                                                 \
    }                                                                             \
  } while (false)

#define COVA_CHECK_EQ(a, b)                                                       \
  do {                                                                            \
    /* By value: binding a reference to `*optional` does not extend the           \
       optional temporary's lifetime, so the reference would dangle. */           \
    auto cova_lhs_ = (a);                                                         \
    auto cova_rhs_ = (b);                                                         \
    if (!(cova_lhs_ == cova_rhs_)) {                                              \
      COVA_FAIL_AT("expected equality:\n  left:  " +                              \
                       ::cova::test::format_value(cova_lhs_) +                    \
                       "\n  right: " + ::cova::test::format_value(cova_rhs_),     \
                   #a " == " #b);                                                 \
    }                                                                             \
  } while (false)

#define COVA_CHECK_NE(a, b)                                                       \
  do {                                                                            \
    auto cova_lhs_ = (a);                                                         \
    auto cova_rhs_ = (b);                                                         \
    if (cova_lhs_ == cova_rhs_) {                                                 \
      COVA_FAIL_AT("expected inequality with: " +                                 \
                       ::cova::test::format_value(cova_rhs_),                     \
                   #a " != " #b);                                                 \
    }                                                                             \
  } while (false)

// Fails the test unless `result` holds a value; returns that value.
#define COVA_REQUIRE_OK(result)                                                   \
  ([&]() {                                                                        \
    auto cova_result_ = (result);                                                 \
    if (!cova_result_) {                                                          \
      COVA_FAIL_AT("expected success, got error: " +                              \
                       ::cova::test::format_value(cova_result_.error().to_string()),\
                   #result);                                                      \
    }                                                                             \
    using CovaRequireValue =                                                      \
        std::remove_reference_t<decltype(*cova_result_)>;                         \
    return CovaRequireValue(std::move(*cova_result_));                            \
  }())

// Fails the test unless `result` holds an error; returns that error.
#define COVA_REQUIRE_ERR(result)                                                  \
  ([&]() {                                                                        \
    auto cova_result_ = (result);                                                 \
    if (cova_result_) {                                                           \
      COVA_FAIL_AT("expected an error but the operation succeeded", #result);     \
    }                                                                             \
    using CovaRequireError = std::remove_reference_t<decltype(cova_result_.error())>;\
    return CovaRequireError(cova_result_.error());                                \
  }())

// Runs the in-tree test runner.
#define COVA_TEST_MAIN() \
  int main(int argc, char** argv) { return ::cova::test::run_all(argc, argv).ok() ? 0 : 1; }
