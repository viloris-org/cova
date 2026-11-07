// Cova — minimal in-tree test framework (implementation).
//
// Deliberately tiny: registration happens at static-init time, assertions throw
// AssertionFailure, and the runner isolates each case so one failure never hides
// the rest. Cova must bootstrap without GoogleTest (PRD §16.3).
#include "testing/test.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cstdlib>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

#include "core/common.hpp"

namespace cova::test {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

Registration::Registration(std::string suite, std::string name, std::function<void()> body,
                           std::string file, int line) {
  registry().push_back(
      TestCase{std::move(suite), std::move(name), std::move(body), std::move(file), line});
}

[[noreturn]] void fail_assertion(std::string message, std::string file, int line,
                                 std::string expression) {
  throw AssertionFailure{std::move(message), std::move(file), line, std::move(expression)};
}

std::size_t Suite::passed() const {
  std::size_t count = 0;
  for (const CaseResult& result : results) {
    if (result.passed && !result.skipped) {
      ++count;
    }
  }
  return count;
}

std::size_t Suite::failed() const {
  std::size_t count = 0;
  for (const CaseResult& result : results) {
    if (!result.passed) {
      ++count;
    }
  }
  return count;
}

std::size_t Suite::skipped() const {
  std::size_t count = 0;
  for (const CaseResult& result : results) {
    if (result.skipped) {
      ++count;
    }
  }
  return count;
}

std::string format_value(const std::string& value) { return "\"" + value + "\""; }
std::string format_value(const char* value) { return value == nullptr ? "(null)" : std::string("\"") + value + "\""; }
std::string format_value(std::string_view value) { return "\"" + std::string(value) + "\""; }
std::string format_value(bool value) { return value ? "true" : "false"; }

namespace {

constexpr std::string_view kGreen = "\x1b[32;1m";
constexpr std::string_view kRed = "\x1b[31;1m";
constexpr std::string_view kYellow = "\x1b[33;1m";
constexpr std::string_view kDim = "\x1b[2m";
constexpr std::string_view kReset = "\x1b[0m";

struct Counters {
  std::size_t passed = 0;
  std::size_t failed = 0;
  std::size_t skipped = 0;
};

bool contains_filter(const std::string& name, const std::string& filter) {
  return filter.empty() || name.find(filter) != std::string::npos;
}

void print_failure(const CaseResult& result, const AssertionFailure* failure, bool verbose) {
  std::cout << "\n  FAILED " << result.full_name;
  if (failure != nullptr && !failure->file.empty()) {
    std::cout << " (" << failure->file << ":" << failure->line << ")";
  }
  std::cout << "\n";
  if (!result.message.empty()) {
    std::size_t begin = 0;
    const std::string& message = result.message;
    while (begin <= message.size()) {
      const std::size_t end = message.find('\n', begin);
      const std::string_view line =
          end == std::string::npos ? std::string_view(message).substr(begin)
                                   : std::string_view(message).substr(begin, end - begin);
      std::cout << "    " << line << "\n";
      if (end == std::string::npos) {
        break;
      }
      begin = end + 1;
    }
  }
  (void)verbose;
}

}  // namespace

Suite run_all(const Options& options) {
  Suite suite;
  std::vector<TestCase>& cases = registry();
  // Deterministic execution order regardless of static-init order across TUs.
  std::vector<const TestCase*> ordered;
  ordered.reserve(cases.size());
  for (const TestCase& test_case : cases) {
    ordered.push_back(&test_case);
  }
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const TestCase* a, const TestCase* b) {
                     const std::string left = a->suite + "." + a->name;
                     const std::string right = b->suite + "." + b->name;
                     return left < right;
                   });

  std::vector<const TestCase*> selected;
  for (const TestCase* test_case : ordered) {
    if (contains_filter(test_case->suite + "." + test_case->name, options.filter)) {
      selected.push_back(test_case);
    }
  }

  if (options.list_only) {
    for (const TestCase* test_case : selected) {
      std::cout << test_case->suite << "." << test_case->name << "\n";
    }
    return suite;
  }

  std::cout << "running " << selected.size() << " test";
  if (selected.size() != 1) {
    std::cout << "s";
  }
  if (!options.filter.empty()) {
    std::cout << " matching \"" << options.filter << "\"";
  }
  std::cout << "\n";

  for (const TestCase* test_case : selected) {
    CaseResult result;
    result.full_name = test_case->suite + "." + test_case->name;
    const auto started = std::chrono::steady_clock::now();
    AssertionFailure failure;
    bool has_failure = false;
    try {
      test_case->body();
    } catch (const AssertionFailure& assertion) {
      failure = assertion;
      has_failure = true;
      result.passed = false;
    } catch (const std::exception& exception) {
      failure.message = std::string("uncaught exception: ") + exception.what();
      has_failure = true;
      result.passed = false;
    } catch (...) {
      failure.message = "uncaught non-standard exception";
      has_failure = true;
      result.passed = false;
    }
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    if (has_failure) {
      result.message = failure.message;
    }

    if (options.tap || options.verbose) {
      if (result.passed) {
        std::cout << "ok " << result.full_name << " (" << result.duration.count() << "ms)\n";
      } else {
        std::cout << "not ok " << result.full_name << "\n";
      }
    } else if (!result.passed) {
      // Failures are always reported, even in compact mode.
    } else {
      std::cout << ".";
      std::cout.flush();
    }

    if (!result.passed) {
      print_failure(result, has_failure ? &failure : nullptr, options.verbose);
      if (!options.tap && !options.verbose) {
        std::cout << "\n";
      }
    }
    suite.results.push_back(std::move(result));
  }

  if (!options.tap && !options.verbose) {
    std::cout << "\n";
  }

  std::cout << "\n" << suite.passed() << " passed, " << suite.failed() << " failed";
  if (suite.skipped() != 0) {
    std::cout << ", " << suite.skipped() << " skipped";
  }
  std::cout << "\n";
  return suite;
}

Suite run_all(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--list") {
      options.list_only = true;
    } else if (arg == "--verbose" || arg == "-v") {
      options.verbose = true;
    } else if (arg == "--tap") {
      options.tap = true;
    } else if (arg == "--no-color") {
      options.no_color = true;
    } else if (str::starts_with(arg, "--filter=")) {
      options.filter = std::string(arg.substr(std::string_view("--filter=").size()));
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "usage: " << (argc > 0 ? argv[0] : "test") << " [filter] [--list] [--tap] [--verbose]\n";
      std::exit(0);
    } else if (!str::starts_with(arg, "-")) {
      // A bare positional argument is a substring filter; this is what
      // `devbuild.py test-module core <filter>` passes.
      options.filter = std::string(arg);
    }
  }
  return run_all(options);
}

// ---------------------------------------------------------------------------
// TempDir
// ---------------------------------------------------------------------------

namespace {

std::string temp_root() {
  for (const char* variable : {"TMPDIR", "TEMP", "TMP"}) {
    if (const char* value = std::getenv(variable); value != nullptr && value[0] != '\0') {
      return value;
    }
  }
#ifdef _WIN32
  return ".";
#else
  return "/tmp";
#endif
}

int current_process_id() {
#ifdef _WIN32
  return _getpid();
#else
  return static_cast<int>(::getpid());
#endif
}

}  // namespace

TempDir::TempDir() {
  static std::atomic<unsigned long long> counter{0};
  const std::string root = temp_root();
  for (unsigned long long attempt = 0;; ++attempt) {
    std::ostringstream name;
    name << root << "/cova-test-" << current_process_id() << "-"
         << (++counter + attempt * 7919ull);
    std::error_code ec;
    if (std::filesystem::create_directory(name.str(), ec) && !ec) {
      path_ = name.str();
      return;
    }
    if (attempt > 10000) {
      path_ = root + "/cova-test-fallback-" + std::to_string(current_process_id());
      std::filesystem::create_directories(path_);
      return;
    }
  }
}

TempDir::~TempDir() { cleanup(); }

TempDir::TempDir(TempDir&& other) noexcept : path_(std::move(other.path_)) { other.path_.clear(); }

TempDir& TempDir::operator=(TempDir&& other) noexcept {
  if (this != &other) {
    cleanup();
    path_ = std::move(other.path_);
    other.path_.clear();
  }
  return *this;
}

void TempDir::cleanup() noexcept {
  if (path_.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::remove_all(path_, ec);
  path_.clear();
}

std::string TempDir::sub(std::string_view name) const {
  while (!name.empty() && name.front() == '/') {
    name.remove_prefix(1);
  }
  if (path_.empty()) {
    return std::string(name);
  }
  if (name.empty()) {
    return path_;
  }
  return path_ + "/" + std::string(name);
}

// ---------------------------------------------------------------------------
// ScopedEnv
// ---------------------------------------------------------------------------

ScopedEnv::ScopedEnv(std::string name, std::string value) : name_(std::move(name)) {
#ifdef _WIN32
  char* existing = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&existing, &size, name_.c_str()) == 0 && existing != nullptr) {
    old_ = existing;
    had_old_ = true;
    std::free(existing);
  }
  _putenv_s(name_.c_str(), value.c_str());
#else
  if (const char* existing = std::getenv(name_.c_str()); existing != nullptr) {
    old_ = existing;
    had_old_ = true;
  }
  ::setenv(name_.c_str(), value.c_str(), 1);
#endif
}

ScopedEnv::~ScopedEnv() {
#ifdef _WIN32
  if (had_old_) {
    _putenv_s(name_.c_str(), old_.c_str());
  } else {
    _putenv_s(name_.c_str(), "");
  }
#else
  if (had_old_) {
    ::setenv(name_.c_str(), old_.c_str(), 1);
  } else {
    ::unsetenv(name_.c_str());
  }
#endif
}

}  // namespace cova::test
