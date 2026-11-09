// Cova — child process execution (implementation).
//
// POSIX: fork/execvp with pipes. No shell is ever involved, so argv entries are
// passed literally and never re-split (PRD §7.6, §9.4).
//
// Windows uses CreateProcessW with anonymous pipes; see the backend below. Both
// backends share ProcessSpec/ProcessResult, so callers never branch on the host.
#include "core/process/process.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/common.hpp"
#include "core/filesystem/filesystem.hpp"

#ifdef _WIN32

// Windows uses CreateProcessW with anonymous pipes. Arguments are encoded with
// the quoting rules the MSVC runtime and CommandLineToArgvW understand, so an
// argument is never re-split by a shell (PRD §7.6, §9.4). The child is placed in
// a job object, so a timeout terminates the whole process tree, matching the
// POSIX process-group behaviour.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>

#include <atomic>
#include <semaphore>
#include <thread>

namespace cova::proc {
namespace {

constexpr std::chrono::milliseconds kPollSlice{20};

// Owns a Win32 handle and closes it on scope exit.
class WinHandle {
 public:
  WinHandle() = default;
  explicit WinHandle(HANDLE handle) : handle_(handle) {}
  ~WinHandle() { reset(); }

  WinHandle(const WinHandle&) = delete;
  WinHandle& operator=(const WinHandle&) = delete;

  WinHandle(WinHandle&& other) noexcept : handle_(other.release()) {}
  WinHandle& operator=(WinHandle&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  HANDLE get() const { return handle_; }
  bool valid() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
  HANDLE release() {
    const HANDLE value = handle_;
    handle_ = INVALID_HANDLE_VALUE;
    return value;
  }
  void reset(HANDLE value = INVALID_HANDLE_VALUE) {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle_);
    }
    handle_ = value;
  }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::wstring widen(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) {
    return {};
  }
  std::wstring out(static_cast<std::size_t>(length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                        length);
  return out;
}

std::string narrow(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const int length = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
  if (length <= 0) {
    return {};
  }
  std::string out(static_cast<std::size_t>(length), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                        length, nullptr, nullptr);
  return out;
}

std::string windows_error(DWORD code) {
  std::wstring message;
  wchar_t* buffer = nullptr;
  const DWORD length = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  if (length > 0 && buffer != nullptr) {
    message.assign(buffer, length);
    ::LocalFree(buffer);
  }
  const std::string text = str::trim(narrow(message));
  if (text.empty()) {
    return std::to_string(code);
  }
  return std::to_string(code) + " (" + text + ")";
}

bool same_name(std::wstring_view left, std::wstring_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    wchar_t a = left[index];
    wchar_t b = right[index];
    if (a >= L'A' && a <= L'Z') {
      a = static_cast<wchar_t>(a - L'A' + L'a');
    }
    if (b >= L'A' && b <= L'Z') {
      b = static_cast<wchar_t>(b - L'A' + L'a');
    }
    if (a != b) {
      return false;
    }
  }
  return true;
}

// Quotes one argument for a CreateProcessW command line. This is the inverse of
// the MSVC runtime / CommandLineToArgvW rules: backslashes are only special
// before a quote, and a quote is escaped with a backslash.
std::wstring quote_argument(std::wstring_view argument) {
  if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
    return std::wstring(argument);
  }
  std::wstring out;
  out.push_back(L'"');
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
      backslashes = 0;
      continue;
    }
    out.append(backslashes, L'\\');
    backslashes = 0;
    out.push_back(character);
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

std::wstring build_command_line(const Path& program, const std::vector<std::string>& args) {
  std::wstring line = quote_argument(program.native());
  for (const std::string& argument : args) {
    line.push_back(L' ');
    line += quote_argument(widen(argument));
  }
  return line;
}

// Returns the parent environment as name/value pairs. Entries beginning with
// '=' (per-drive current directories) are preserved.
std::vector<std::pair<std::wstring, std::wstring>> current_environment() {
  std::vector<std::pair<std::wstring, std::wstring>> entries;
  wchar_t* block = ::GetEnvironmentStringsW();
  if (block == nullptr) {
    return entries;
  }
  const wchar_t* cursor = block;
  while (*cursor != L'\0') {
    const std::wstring entry(cursor);
    cursor += entry.size() + 1;
    std::size_t equals = entry.find(L'=');
    if (equals == 0) {
      equals = entry.find(L'=', 1);
    }
    if (equals == std::wstring::npos) {
      continue;
    }
    entries.emplace_back(entry.substr(0, equals), entry.substr(equals + 1));
  }
  ::FreeEnvironmentStringsW(block);
  return entries;
}

// Builds the child environment entries from either the explicit environment in
// the spec or the parent environment, then applies removals and overrides.
std::vector<std::pair<std::wstring, std::wstring>> build_environment_entries(
    const ProcessSpec& spec) {
  std::vector<std::pair<std::wstring, std::wstring>> entries;
  if (spec.environment) {
    entries.reserve(spec.environment->size());
    for (const auto& [name, value] : *spec.environment) {
      entries.emplace_back(widen(name), widen(value));
    }
  } else {
    entries = current_environment();
  }

  for (const std::string& removed : spec.env_remove) {
    const std::wstring name = widen(removed);
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&name](const auto& entry) {
                                   return same_name(entry.first, name);
                                 }),
                  entries.end());
  }
  for (const auto& [name_text, value_text] : spec.env_overrides) {
    const std::wstring name = widen(name_text);
    const std::wstring value = widen(value_text);
    auto found = std::find_if(entries.begin(), entries.end(), [&name](const auto& entry) {
      return same_name(entry.first, name);
    });
    if (found == entries.end()) {
      entries.emplace_back(name, value);
    } else {
      found->second = value;
    }
  }
  return entries;
}

std::optional<std::wstring> environment_value(
    const std::vector<std::pair<std::wstring, std::wstring>>& entries, std::wstring_view name) {
  for (const auto& [entry_name, value] : entries) {
    if (same_name(entry_name, name)) {
      return value;
    }
  }
  return std::nullopt;
}

// Builds a sorted, double-NUL-terminated UTF-16 environment block.
std::wstring build_environment(const ProcessSpec& spec) {
  auto entries = build_environment_entries(spec);
  std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
    return str::to_lower(narrow(left.first)) < str::to_lower(narrow(right.first));
  });

  std::wstring block;
  for (const auto& [name, value] : entries) {
    block += name;
    block.push_back(L'=');
    block += value;
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

HANDLE duplicate_inheritable(DWORD which) {
  const HANDLE source = ::GetStdHandle(which);
  if (source == nullptr || source == INVALID_HANDLE_VALUE) {
    return INVALID_HANDLE_VALUE;
  }
  HANDLE copy = INVALID_HANDLE_VALUE;
  if (!::DuplicateHandle(::GetCurrentProcess(), source, ::GetCurrentProcess(), &copy, 0, TRUE,
                         DUPLICATE_SAME_ACCESS)) {
    return INVALID_HANDLE_VALUE;
  }
  return copy;
}

HANDLE open_null(DWORD access) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  return ::CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                       OPEN_EXISTING, 0, nullptr);
}

// Prepares the child's end of a standard stream that must be inherited. Falls
// back to the NUL device when the parent has no usable handle.
HANDLE inherited_std_handle(DWORD which, DWORD fallback_access, bool& owned) {
  const HANDLE copy = duplicate_inheritable(which);
  if (copy != INVALID_HANDLE_VALUE) {
    owned = true;
    return copy;
  }
  const HANDLE original = ::GetStdHandle(which);
  if (original != nullptr && original != INVALID_HANDLE_VALUE) {
    // Console handles are inherited without duplication; do not close this.
    owned = false;
    return original;
  }
  const HANDLE null_handle = open_null(fallback_access);
  owned = null_handle != INVALID_HANDLE_VALUE;
  return null_handle;
}

std::optional<Path> find_on_path(std::string_view executable, std::wstring_view path_value) {
  if (executable.empty()) {
    return std::nullopt;
  }
  std::vector<std::string> names{std::string(executable)};
  if (Path(executable).extension().empty()) {
    names.push_back(std::string(executable) + ".exe");
  }
  std::size_t start = 0;
  while (start <= path_value.size()) {
    const std::size_t end = path_value.find(L';', start);
    const std::wstring_view directory =
        path_value.substr(start, end == std::wstring_view::npos ? path_value.size() - start
                                                                : end - start);
    for (const std::string& name : names) {
      const Path candidate = Path(std::wstring(directory)) / name;
      if (fs::is_regular_file(candidate)) {
        return candidate;
      }
    }
    if (end == std::wstring_view::npos) {
      break;
    }
    start = end + 1;
  }
  return std::nullopt;
}

Result<Path> resolve_program(const Path& program, std::wstring_view path_value) {
  if (program.empty()) {
    return std::unexpected(Error::make("cova::proc::invalid", "empty program path"));
  }
  if (program.has_parent_path()) {
    // An explicit path is used verbatim (adding the conventional .exe suffix
    // when the caller omitted it), not searched on PATH.
    for (const std::string& name : {program.string(), program.string() + ".exe"}) {
      const Path candidate(name);
      if (fs::is_regular_file(candidate)) {
        return candidate;
      }
    }
    return std::unexpected(Error::make("cova::proc::not_found", "program does not exist")
                               .with("program: " + program.string()));
  }
  const std::optional<Path> found = find_on_path(program.string(), path_value);
  if (!found) {
    return std::unexpected(Error::make("cova::proc::not_found", "program not found on PATH")
                               .with("program: " + program.string()));
  }
  return *found;
}

void drain_pipe(HANDLE pipe, std::string& sink) {
  std::array<char, 65536> buffer{};
  for (;;) {
    DWORD available = 0;
    if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) {
      return;
    }
    const DWORD wanted = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
    DWORD read = 0;
    if (!::ReadFile(pipe, buffer.data(), wanted, &read, nullptr) || read == 0) {
      return;
    }
    sink.append(buffer.data(), static_cast<std::size_t>(read));
  }
}

}  // namespace

std::vector<std::pair<std::string, std::string>> process_environment() {
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& [name, value] : current_environment()) {
    out.emplace_back(narrow(name), narrow(value));
  }
  std::sort(out.begin(), out.end(), [](const auto& left, const auto& right) {
    return str::to_lower(left.first) < str::to_lower(right.first);
  });
  return out;
}

std::string ProcessResult::stdout_trimmed() const { return str::trim(stdout_text); }
std::string ProcessResult::stderr_trimmed() const { return str::trim(stderr_text); }

std::string ProcessResult::to_string() const {
  std::string out = "exit=" + std::to_string(exit_code);
  if (timed_out) {
    out += " timed_out";
  }
  out += " duration=" + std::to_string(duration.count()) + "ms";
  return out;
}

Result<ProcessResult> run(const ProcessSpec& spec) {
  // Build the environment first: program lookup must use the child's PATH, not
  // the parent's, when the caller supplied an explicit environment.
  const auto environment_entries = build_environment_entries(spec);
  const std::wstring path_value =
      environment_value(environment_entries, L"PATH").value_or(std::wstring());
  auto resolved_result = resolve_program(spec.program, path_value);
  if (!resolved_result) {
    return std::unexpected(std::move(resolved_result.error()));
  }
  const Path resolved = std::move(*resolved_result);

  if (spec.cwd && !fs::is_directory(*spec.cwd)) {
    return std::unexpected(Error::make("cova::proc::cwd", "working directory does not exist")
                               .with("cwd: " + spec.cwd->string()));
  }

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  const bool needs_stdin_pipe = spec.stdin_data.has_value() && !spec.inherit_stdin;

  WinHandle stdout_read;
  WinHandle stderr_read;
  WinHandle stdin_write;

  HANDLE child_in = INVALID_HANDLE_VALUE;
  HANDLE child_out = INVALID_HANDLE_VALUE;
  HANDLE child_err = INVALID_HANDLE_VALUE;
  bool own_in = false;
  bool own_out = false;
  bool own_err = false;

  auto close_child_ends = [&]() {
    if (own_in && child_in != INVALID_HANDLE_VALUE) {
      ::CloseHandle(child_in);
    }
    if (own_out && child_out != INVALID_HANDLE_VALUE) {
      ::CloseHandle(child_out);
    }
    if (own_err && child_err != INVALID_HANDLE_VALUE) {
      ::CloseHandle(child_err);
    }
    child_in = child_out = child_err = INVALID_HANDLE_VALUE;
    own_in = own_out = own_err = false;
  };

  auto setup_captured_output = [&](DWORD which, bool inherit, WinHandle& read_end,
                                   HANDLE& child_end, bool& child_owns, const char* label) {
    if (inherit) {
      child_end = inherited_std_handle(which, GENERIC_WRITE, child_owns);
      if (child_end == INVALID_HANDLE_VALUE) {
        return Status(std::unexpected(
            Error::make("cova::proc::pipe", std::string("cannot inherit ") + label)
                .with("windows_error: " + windows_error(::GetLastError()))));
      }
      return ok();
    }
    HANDLE raw_read = INVALID_HANDLE_VALUE;
    HANDLE raw_write = INVALID_HANDLE_VALUE;
    if (!::CreatePipe(&raw_read, &raw_write, &attributes, 0)) {
      return Status(std::unexpected(
          Error::make("cova::proc::pipe", std::string("cannot create ") + label)
              .with("windows_error: " + windows_error(::GetLastError()))));
    }
    if (!::SetHandleInformation(raw_read, HANDLE_FLAG_INHERIT, 0)) {
      ::CloseHandle(raw_read);
      ::CloseHandle(raw_write);
      return Status(std::unexpected(
          Error::make("cova::proc::pipe", std::string("cannot secure ") + label)
              .with("windows_error: " + windows_error(::GetLastError()))));
    }
    read_end.reset(raw_read);
    child_end = raw_write;
    child_owns = true;
    return ok();
  };

  if (Status prepared =
          setup_captured_output(STD_OUTPUT_HANDLE, spec.inherit_stdout, stdout_read, child_out,
                                own_out, "stdout pipe");
      !prepared) {
    close_child_ends();
    return std::unexpected(std::move(prepared.error()));
  }
  if (Status prepared =
          setup_captured_output(STD_ERROR_HANDLE, spec.inherit_stderr, stderr_read, child_err,
                                own_err, "stderr pipe");
      !prepared) {
    close_child_ends();
    return std::unexpected(std::move(prepared.error()));
  }

  if (spec.inherit_stdin) {
    child_in = inherited_std_handle(STD_INPUT_HANDLE, GENERIC_READ, own_in);
    if (child_in == INVALID_HANDLE_VALUE) {
      close_child_ends();
      return std::unexpected(Error::make("cova::proc::pipe", "cannot inherit stdin")
                                 .with("windows_error: " + windows_error(::GetLastError())));
    }
  } else if (needs_stdin_pipe) {
    HANDLE raw_read = INVALID_HANDLE_VALUE;
    HANDLE raw_write = INVALID_HANDLE_VALUE;
    if (!::CreatePipe(&raw_read, &raw_write, &attributes, 0)) {
      close_child_ends();
      return std::unexpected(Error::make("cova::proc::pipe", "cannot create stdin pipe")
                                 .with("windows_error: " + windows_error(::GetLastError())));
    }
    if (!::SetHandleInformation(raw_write, HANDLE_FLAG_INHERIT, 0)) {
      ::CloseHandle(raw_read);
      ::CloseHandle(raw_write);
      close_child_ends();
      return std::unexpected(Error::make("cova::proc::pipe", "cannot secure stdin pipe")
                                 .with("windows_error: " + windows_error(::GetLastError())));
    }
    child_in = raw_read;
    own_in = true;
    stdin_write.reset(raw_write);
  } else {
    child_in = open_null(GENERIC_READ);
    if (child_in == INVALID_HANDLE_VALUE) {
      close_child_ends();
      return std::unexpected(Error::make("cova::proc::pipe", "cannot open the NUL device")
                                 .with("windows_error: " + windows_error(::GetLastError())));
    }
    own_in = true;
  }

  std::wstring command_line = build_command_line(resolved, spec.args);
  std::wstring environment = build_environment(spec);

  STARTUPINFOEXW startup{};
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = child_in;
  startup.StartupInfo.hStdOutput = child_out;
  startup.StartupInfo.hStdError = child_err;
  startup.StartupInfo.cb = sizeof(STARTUPINFOW);

  // Restrict inheritance to exactly the three standard handles. Without this,
  // a child would inherit the pipe ends of every concurrently running action,
  // delaying EOF for them until this child exits.
  std::array<HANDLE, 3> inherited_handles{};
  std::size_t inherited_count = 0;
  for (const HANDLE handle : {child_in, child_out, child_err}) {
    if (handle == INVALID_HANDLE_VALUE) {
      continue;
    }
    bool duplicate = false;
    for (std::size_t index = 0; index < inherited_count; ++index) {
      duplicate = duplicate || inherited_handles[index] == handle;
    }
    if (!duplicate) {
      inherited_handles[inherited_count++] = handle;
    }
  }

  LPPROC_THREAD_ATTRIBUTE_LIST attribute_list = nullptr;
  bool extended_startup = false;
  SIZE_T attributes_size = 0;
  ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributes_size);
  if (attributes_size > 0) {
    attribute_list = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        ::HeapAlloc(::GetProcessHeap(), 0, attributes_size));
    if (attribute_list != nullptr) {
      const bool initialized =
          ::InitializeProcThreadAttributeList(attribute_list, 1, 0, &attributes_size);
      if (initialized &&
          ::UpdateProcThreadAttribute(attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      inherited_handles.data(),
                                      inherited_count * sizeof(HANDLE), nullptr, nullptr)) {
        extended_startup = true;
        startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
      } else {
        if (initialized) {
          ::DeleteProcThreadAttributeList(attribute_list);
        }
        ::HeapFree(::GetProcessHeap(), 0, attribute_list);
        attribute_list = nullptr;
      }
    }
  }

  PROCESS_INFORMATION information{};
  const wchar_t* working_directory = spec.cwd ? spec.cwd->native().c_str() : nullptr;
  DWORD creation_flags = CREATE_UNICODE_ENVIRONMENT;
  if (extended_startup) {
    creation_flags |= EXTENDED_STARTUPINFO_PRESENT;
  }
  const BOOL created = ::CreateProcessW(
      resolved.native().c_str(), command_line.data(), nullptr, nullptr, TRUE, creation_flags,
      environment.data(), working_directory, &startup.StartupInfo, &information);
  if (attribute_list != nullptr) {
    ::DeleteProcThreadAttributeList(attribute_list);
    ::HeapFree(::GetProcessHeap(), 0, attribute_list);
  }
  if (!created) {
    const DWORD error = ::GetLastError();
    close_child_ends();
    return std::unexpected(Error::make("cova::proc::spawn", "cannot start process")
                               .with("program: " + resolved.string())
                               .with("windows_error: " + windows_error(error)));
  }
  close_child_ends();

  WinHandle process(information.hProcess);
  WinHandle process_thread(information.hThread);

  // Timeouts kill the whole tree; when job assignment is refused (for example,
  // because the parent already sits in a restrictive job), fall back to killing
  // only the direct child.
  WinHandle job;
  if (HANDLE raw_job = ::CreateJobObjectW(nullptr, nullptr); raw_job != nullptr) {
    if (::AssignProcessToJobObject(raw_job, process.get())) {
      job.reset(raw_job);
    } else {
      ::CloseHandle(raw_job);
    }
  }

  // Feed stdin from a writer thread: a large payload must never block the
  // parent while it is still draining the child's stdout. The thread publishes
  // a real Win32 thread handle because MinGW's std::thread::native_handle() is
  // a thread id rather than a HANDLE.
  std::binary_semaphore writer_ready{0};
  std::atomic<HANDLE> writer_thread{nullptr};
  std::thread writer;
  if (stdin_write.valid()) {
    HANDLE write_handle = stdin_write.release();
    const std::string* data = &*spec.stdin_data;
    writer = std::thread([&, write_handle, data]() {
      HANDLE self = nullptr;
      if (::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(),
                            ::GetCurrentProcess(), &self, THREAD_TERMINATE, FALSE, 0)) {
        writer_thread.store(self);
      }
      writer_ready.release();
      std::size_t offset = 0;
      while (offset < data->size()) {
        const DWORD chunk =
            static_cast<DWORD>(std::min<std::size_t>(data->size() - offset, 65536));
        DWORD written = 0;
        if (!::WriteFile(write_handle, data->data() + offset, chunk, &written, nullptr) ||
            written == 0) {
          break;
        }
        offset += written;
      }
      ::CloseHandle(write_handle);
    });
  }

  ProcessResult result;
  const auto started = std::chrono::steady_clock::now();
  const auto deadline = spec.timeout.count() > 0
                            ? started + spec.timeout
                            : std::chrono::steady_clock::time_point::max();
  bool timed_out = false;
  DWORD exit_code = 0;

  for (;;) {
    if (stdout_read.valid()) {
      drain_pipe(stdout_read.get(), result.stdout_text);
    }
    if (stderr_read.valid()) {
      drain_pipe(stderr_read.get(), result.stderr_text);
    }

    const auto now = std::chrono::steady_clock::now();
    if (spec.timeout.count() > 0 && now >= deadline) {
      timed_out = true;
      if (job.valid()) {
        ::TerminateJobObject(job.get(), 1);
      } else {
        ::TerminateProcess(process.get(), 1);
      }
      ::WaitForSingleObject(process.get(), INFINITE);
      ::GetExitCodeProcess(process.get(), &exit_code);
      break;
    }

    DWORD wait_ms = static_cast<DWORD>(kPollSlice.count());
    if (spec.timeout.count() > 0) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      if (remaining.count() <= 0) {
        wait_ms = 0;
      } else if (remaining.count() < static_cast<long long>(wait_ms)) {
        wait_ms = static_cast<DWORD>(remaining.count());
      }
    }

    const DWORD status = ::WaitForSingleObject(process.get(), wait_ms);
    if (status == WAIT_OBJECT_0) {
      ::GetExitCodeProcess(process.get(), &exit_code);
      break;
    }
    if (status == WAIT_FAILED) {
      ::GetExitCodeProcess(process.get(), &exit_code);
      break;
    }
  }

  if (stdout_read.valid()) {
    drain_pipe(stdout_read.get(), result.stdout_text);
  }
  if (stderr_read.valid()) {
    drain_pipe(stderr_read.get(), result.stderr_text);
  }

  if (writer.joinable()) {
    // A child that never read stdin can leave the writer blocked on a full
    // pipe; cancel the outstanding write before joining so run() always returns.
    writer_ready.acquire();
    const HANDLE thread_handle = writer_thread.load();
    if (thread_handle != nullptr) {
      ::CancelSynchronousIo(thread_handle);
    }
    writer.join();
    if (thread_handle != nullptr) {
      ::CloseHandle(thread_handle);
    }
  }

  result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);

  if (timed_out) {
    result.timed_out = true;
    result.exit_code = -1;
    return result;
  }
  result.exit_code = static_cast<int>(exit_code);
  return result;
}

Result<ProcessResult> run(const Path& program, const std::vector<std::string>& args,
                          const std::optional<Path>& cwd) {
  ProcessSpec spec;
  spec.program = program;
  spec.args = args;
  spec.cwd = cwd;
  return run(spec);
}

Result<ProcessResult> run_line(const std::string& command_line) {
  std::vector<std::string> parts = str::split_whitespace_quoted(command_line);
  if (parts.empty()) {
    return std::unexpected(Error::make("cova::proc::empty", "empty command line"));
  }
  Path program = parts.front();
  parts.erase(parts.begin());
  return run(program, parts, std::nullopt);
}

Result<std::string> capture(const Path& program, const std::vector<std::string>& args,
                            const std::optional<Path>& cwd) {
  auto result = run(program, args, cwd);
  if (!result) {
    return std::unexpected(std::move(result.error()));
  }
  if (!result->success()) {
    Error error = Error::make("cova::proc::failed", "command failed");
    ProcessSpec described;
    described.program = program;
    described.args = args;
    error.with("command: " + format_command_line(described));
    error.with("exit_code: " + std::to_string(result->exit_code));
    if (!result->stderr_text.empty()) {
      error.with("stderr: " + result->stderr_trimmed());
    }
    return std::unexpected(std::move(error));
  }
  return result->stdout_text;
}

bool is_executable(const Path& program) { return fs::is_regular_file(program); }

std::string format_command_line(const ProcessSpec& spec) {
  std::string out = spec.program.string();
  for (const std::string& arg : spec.args) {
    out.push_back(' ');
    if (arg.find(' ') != std::string::npos || arg.find('\t') != std::string::npos) {
      out += str::quote(arg);
    } else {
      out += arg;
    }
  }
  return out;
}

}  // namespace cova::proc

#else  // POSIX

#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace cova::proc {
namespace {

constexpr std::chrono::milliseconds kPollSlice{20};

void close_fd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

// Builds the child environment entries from either the explicit environment in
// the spec or the parent environment, then applies removals and overrides.
std::vector<std::pair<std::string, std::string>> build_environment_entries(
    const ProcessSpec& spec) {
  std::vector<std::pair<std::string, std::string>> entries;
  if (spec.environment) {
    entries = *spec.environment;
  } else {
    for (char** env = environ; env != nullptr && *env != nullptr; ++env) {
      const std::string_view line(*env);
      const std::size_t equals = line.find('=');
      const std::string name(line.substr(0, equals));
      const std::string value = equals == std::string_view::npos
                                    ? std::string()
                                    : std::string(line.substr(equals + 1));
      entries.emplace_back(name, value);
    }
  }

  auto find = [&entries](std::string_view name) -> auto {
    return std::find_if(entries.begin(), entries.end(),
                        [name](const auto& entry) { return entry.first == name; });
  };

  for (const std::string& removed : spec.env_remove) {
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&removed](const auto& entry) { return entry.first == removed; }),
                  entries.end());
  }
  for (const auto& [name, value] : spec.env_overrides) {
    auto it = find(name);
    if (it == entries.end()) {
      entries.emplace_back(name, value);
    } else {
      it->second = value;
    }
  }
  return entries;
}

std::optional<std::string> environment_value(
    const std::vector<std::pair<std::string, std::string>>& entries, std::string_view name) {
  for (const auto& [entry_name, value] : entries) {
    if (entry_name == name) {
      return value;
    }
  }
  return std::nullopt;
}

// Builds the environment block for execve from the entries. Kept deterministic
// in ordering.
std::vector<std::string> build_environment(const ProcessSpec& spec) {
  const auto entries = build_environment_entries(spec);
  std::vector<std::string> block;
  block.reserve(entries.size());
  for (const auto& [name, value] : entries) {
    block.push_back(name + "=" + value);
  }
  return block;
}

// Resolves the program: an explicit path is used verbatim (must be executable),
// a bare name is looked up on the child's PATH before forking so the error is
// precise.
Result<std::string> resolve_program(const Path& program, std::string_view path_value) {
  const std::string text = program.string();
  if (text.empty()) {
    return std::unexpected(Error::make("cova::proc::invalid", "empty program path"));
  }
  if (text.find('/') != std::string::npos) {
    if (!fs::is_regular_file(program)) {
      return std::unexpected(Error::make("cova::proc::not_found", "program does not exist")
                                 .with("program: " + text));
    }
    if (::access(text.c_str(), X_OK) != 0) {
      return std::unexpected(Error::make("cova::proc::not_executable", "program is not executable")
                                 .with("program: " + text));
    }
    return text;
  }
  for (const std::string& directory : str::split(path_value, ':')) {
    const Path base = directory.empty() ? Path(".") : Path(directory);
    const Path candidate = base / program;
    if (fs::is_regular_file(candidate) && ::access(candidate.c_str(), X_OK) == 0) {
      return candidate.string();
    }
  }
  return std::unexpected(Error::make("cova::proc::not_found", "program not found on PATH")
                             .with("program: " + text));
}

}  // namespace

std::vector<std::pair<std::string, std::string>> process_environment() {
  std::vector<std::pair<std::string, std::string>> out;
  for (char** env = environ; env != nullptr && *env != nullptr; ++env) {
    const std::string_view line(*env);
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
      continue;
    }
    out.emplace_back(std::string(line.substr(0, equals)), std::string(line.substr(equals + 1)));
  }
  std::sort(out.begin(), out.end(), [](const auto& left, const auto& right) {
    return left.first < right.first;
  });
  return out;
}

std::string ProcessResult::stdout_trimmed() const { return str::trim(stdout_text); }
std::string ProcessResult::stderr_trimmed() const { return str::trim(stderr_text); }

std::string ProcessResult::to_string() const {
  std::string out = "exit=" + std::to_string(exit_code);
  if (timed_out) {
    out += " timed_out";
  }
  out += " duration=" + std::to_string(duration.count()) + "ms";
  return out;
}

Result<ProcessResult> run(const ProcessSpec& spec) {
  // Build the environment first: program lookup must use the child's PATH, not
  // the parent's, when the caller supplied an explicit environment.
  const auto environment_entries = build_environment_entries(spec);
  const std::string path_value =
      environment_value(environment_entries, "PATH").value_or(std::string());
  auto resolved = resolve_program(spec.program, path_value);
  if (!resolved) {
    return std::unexpected(std::move(resolved.error()));
  }

  // Validate the working directory before forking so the caller gets a precise
  // error rather than a generic exec failure.
  if (spec.cwd && !fs::is_directory(*spec.cwd)) {
    return std::unexpected(Error::make("cova::proc::cwd", "working directory does not exist")
                               .with("cwd: " + spec.cwd->string()));
  }

  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  int stdin_pipe[2] = {-1, -1};
  bool needs_stdin_pipe = spec.stdin_data.has_value() && !spec.inherit_stdin;

  if (!spec.inherit_stdout) {
    if (::pipe(stdout_pipe) != 0) {
      return std::unexpected(Error::make("cova::proc::pipe", "cannot create stdout pipe")
                                 .with("errno: " + std::string(std::strerror(errno))));
    }
  }
  if (!spec.inherit_stderr) {
    if (::pipe(stderr_pipe) != 0) {
      close_fd(stdout_pipe[0]);
      close_fd(stdout_pipe[1]);
      return std::unexpected(Error::make("cova::proc::pipe", "cannot create stderr pipe")
                                 .with("errno: " + std::string(std::strerror(errno))));
    }
  }
  if (needs_stdin_pipe) {
    if (::pipe(stdin_pipe) != 0) {
      close_fd(stdout_pipe[0]);
      close_fd(stdout_pipe[1]);
      close_fd(stderr_pipe[0]);
      close_fd(stderr_pipe[1]);
      return std::unexpected(Error::make("cova::proc::pipe", "cannot create stdin pipe")
                                 .with("errno: " + std::string(std::strerror(errno))));
    }
  }

  // Everything the child needs must be owned by this frame; the child only reads.
  std::vector<std::string> environment = build_environment(spec);
  std::vector<char*> envp;
  envp.reserve(environment.size() + 1);
  for (std::string& entry : environment) {
    envp.push_back(entry.data());
  }
  envp.push_back(nullptr);

  std::vector<std::string> argv_storage;
  argv_storage.push_back(*resolved);
  for (const std::string& arg : spec.args) {
    argv_storage.push_back(arg);
  }
  std::vector<char*> argv;
  argv.reserve(argv_storage.size() + 1);
  for (std::string& arg : argv_storage) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);

  const auto started = std::chrono::steady_clock::now();
  const pid_t pid = ::fork();
  if (pid < 0) {
    close_fd(stdout_pipe[0]);
    close_fd(stdout_pipe[1]);
    close_fd(stderr_pipe[0]);
    close_fd(stderr_pipe[1]);
    close_fd(stdin_pipe[0]);
    close_fd(stdin_pipe[1]);
    return std::unexpected(Error::make("cova::proc::fork", "cannot fork")
                               .with("errno: " + std::string(std::strerror(errno))));
  }

  if (pid == 0) {
    // --- child ---
    // Own process group so a timeout can SIGKILL the whole tree it spawns.
    ::setpgid(0, 0);

    if (spec.cwd && ::chdir(spec.cwd->c_str()) != 0) {
      ::_exit(127);
    }

    if (spec.inherit_stdin) {
      // Our stdin is already the child's stdin.
    } else if (needs_stdin_pipe) {
      ::dup2(stdin_pipe[0], STDIN_FILENO);
      close_fd(stdin_pipe[0]);
      close_fd(stdin_pipe[1]);
    } else {
      const int devnull = ::open("/dev/null", O_RDONLY);
      if (devnull >= 0) {
        ::dup2(devnull, STDIN_FILENO);
        ::close(devnull);
      }
    }

    if (!spec.inherit_stdout) {
      ::dup2(stdout_pipe[1], STDOUT_FILENO);
      close_fd(stdout_pipe[0]);
      close_fd(stdout_pipe[1]);
    }
    if (!spec.inherit_stderr) {
      ::dup2(stderr_pipe[1], STDERR_FILENO);
      close_fd(stderr_pipe[0]);
      close_fd(stderr_pipe[1]);
    }

    ::execve(argv[0], argv.data(), envp.data());
    // If exec fails, report through the dedicated exit code.
    ::_exit(127);
  }

  // --- parent ---
  ::setpgid(pid, pid);  // race-free with the child's own setpgid call
  close_fd(stdout_pipe[1]);
  close_fd(stderr_pipe[1]);
  close_fd(stdin_pipe[0]);

  // Non-blocking read ends: `drain` must return at EAGAIN instead of blocking,
  // otherwise a chatty child that never closes its pipes would deadlock us.
  auto set_nonblocking = [](int fd) {
    if (fd < 0) {
      return;
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
      ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
  };
  set_nonblocking(stdout_pipe[0]);
  set_nonblocking(stderr_pipe[0]);

  // Streaming stdin: bounded writes driven by POLLOUT so a large payload can
  // never block the parent while the child is still starting up.
  std::size_t stdin_offset = 0;
  const std::size_t stdin_size = spec.stdin_data ? spec.stdin_data->size() : 0;
  bool stdin_closed = !needs_stdin_pipe;
  if (!needs_stdin_pipe) {
    close_fd(stdin_pipe[1]);
  }
  auto pump_stdin = [&]() {
    if (stdin_closed || stdin_pipe[1] < 0) {
      return;
    }
    while (stdin_offset < stdin_size) {
      const std::size_t remaining = stdin_size - stdin_offset;
      const std::size_t chunk = std::min<std::size_t>(remaining, 65536);
      const ssize_t n = ::write(stdin_pipe[1], spec.stdin_data->data() + stdin_offset, chunk);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;  // EPIPE: the child is not reading; that is the child's choice
      }
      stdin_offset += static_cast<std::size_t>(n);
      break;  // one bounded chunk per readiness notification
    }
    if (stdin_offset >= stdin_size) {
      close_fd(stdin_pipe[1]);
      stdin_closed = true;
    }
  };

  ProcessResult result;
  bool timed_out = false;
  bool child_exited = false;
  int wait_status = 0;
  std::array<char, 65536> buffer{};
  const auto deadline = spec.timeout.count() > 0 ? started + spec.timeout
                                                 : std::chrono::steady_clock::time_point::max();

  auto drain = [&](int fd, std::string& sink) {
    for (;;) {
      const ssize_t n = ::read(fd, buffer.data(), buffer.size());
      if (n > 0) {
        sink.append(buffer.data(), static_cast<std::size_t>(n));
        continue;
      }
      if (n < 0 && errno == EINTR) {
        continue;
      }
      break;  // EOF or EAGAIN
    }
  };

  while (!child_exited) {
    if (spec.timeout.count() > 0 && std::chrono::steady_clock::now() >= deadline) {
      timed_out = true;
      ::kill(-pid, SIGKILL);
      ::kill(pid, SIGKILL);
      break;
    }

    std::array<struct pollfd, 3> fds{};
    nfds_t count = 0;
    int stdout_slot = -1;
    int stderr_slot = -1;
    int stdin_slot = -1;
    if (stdout_pipe[0] >= 0) {
      stdout_slot = static_cast<int>(count);
      fds[count++] = {stdout_pipe[0], static_cast<short>(POLLIN), 0};
    }
    if (stderr_pipe[0] >= 0) {
      stderr_slot = static_cast<int>(count);
      fds[count++] = {stderr_pipe[0], static_cast<short>(POLLIN), 0};
    }
    if (!stdin_closed && stdin_pipe[1] >= 0) {
      stdin_slot = static_cast<int>(count);
      fds[count++] = {stdin_pipe[1], static_cast<short>(POLLOUT), 0};
    }

    std::chrono::milliseconds wait_for = kPollSlice;
    if (spec.timeout.count() > 0) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      if (remaining < wait_for) {
        wait_for = remaining.count() > 0 ? remaining : std::chrono::milliseconds(0);
      }
    }

    const int ready = ::poll(fds.data(), count, static_cast<int>(wait_for.count()));
    if (ready < 0 && errno != EINTR) {
      break;
    }
    if (ready > 0) {
      if (stdout_slot >= 0 && (fds[stdout_slot].revents & (POLLIN | POLLHUP)) != 0) {
        drain(stdout_pipe[0], result.stdout_text);
        if ((fds[stdout_slot].revents & POLLHUP) != 0) {
          drain(stdout_pipe[0], result.stdout_text);
          close_fd(stdout_pipe[0]);
        }
      }
      if (stderr_slot >= 0 && (fds[stderr_slot].revents & (POLLIN | POLLHUP)) != 0) {
        drain(stderr_pipe[0], result.stderr_text);
        if ((fds[stderr_slot].revents & POLLHUP) != 0) {
          drain(stderr_pipe[0], result.stderr_text);
          close_fd(stderr_pipe[0]);
        }
      }
      if (stdin_slot >= 0 && (fds[stdin_slot].revents & (POLLOUT | POLLHUP | POLLERR)) != 0) {
        pump_stdin();
      }
    } else {
      // No file descriptors ready: the child made no progress, so check whether
      // it is gone. `waitpid` with WNOHANG never blocks.
      pid_t waited = ::waitpid(pid, &wait_status, WNOHANG);
      if (waited == pid) {
        child_exited = true;
        break;
      }
    }
  }

  if (!child_exited) {
    // Either we timed out, or the pipes closed before the process exited.
    for (;;) {
      const pid_t waited = ::waitpid(pid, &wait_status, 0);
      if (waited == pid) {
        child_exited = true;
        break;
      }
      if (waited < 0 && errno != EINTR) {
        break;
      }
    }
  }

  // Collect whatever output remains buffered in the pipes.
  if (stdout_pipe[0] >= 0) {
    drain(stdout_pipe[0], result.stdout_text);
    close_fd(stdout_pipe[0]);
  }
  if (stderr_pipe[0] >= 0) {
    drain(stderr_pipe[0], result.stderr_text);
    close_fd(stderr_pipe[0]);
  }
  close_fd(stdin_pipe[1]);

  result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);

  if (timed_out) {
    result.timed_out = true;
    result.exit_code = -1;
    return result;
  }

  if (WIFEXITED(wait_status)) {
    result.exit_code = WEXITSTATUS(wait_status);
    if (result.exit_code == 127) {
      return std::unexpected(Error::make("cova::proc::exec", "failed to execute program")
                                 .with("program: " + *resolved)
                                 .with("stderr: " + result.stderr_trimmed()));
    }
  } else if (WIFSIGNALED(wait_status)) {
    result.exit_code = 128 + WTERMSIG(wait_status);
  } else {
    result.exit_code = -1;
  }
  return result;
}

Result<ProcessResult> run(const Path& program, const std::vector<std::string>& args,
                          const std::optional<Path>& cwd) {
  ProcessSpec spec;
  spec.program = program;
  spec.args = args;
  spec.cwd = cwd;
  return run(spec);
}

Result<ProcessResult> run_line(const std::string& command_line) {
  std::vector<std::string> parts = str::split_whitespace_quoted(command_line);
  if (parts.empty()) {
    return std::unexpected(Error::make("cova::proc::empty", "empty command line"));
  }
  Path program = parts.front();
  parts.erase(parts.begin());
  return run(program, parts, std::nullopt);
}

Result<std::string> capture(const Path& program, const std::vector<std::string>& args,
                            const std::optional<Path>& cwd) {
  auto result = run(program, args, cwd);
  if (!result) {
    return std::unexpected(std::move(result.error()));
  }
  if (!result->success()) {
    Error error = Error::make("cova::proc::failed", "command failed");
    ProcessSpec described;
    described.program = program;
    described.args = args;
    error.with("command: " + format_command_line(described));
    error.with("exit_code: " + std::to_string(result->exit_code));
    if (!result->stderr_text.empty()) {
      error.with("stderr: " + result->stderr_trimmed());
    }
    return std::unexpected(std::move(error));
  }
  return result->stdout_text;
}

bool is_executable(const Path& program) {
  if (!fs::is_regular_file(program)) {
    return false;
  }
  return ::access(program.c_str(), X_OK) == 0;
}

std::string format_command_line(const ProcessSpec& spec) {
  std::string out = spec.program.string();
  for (const std::string& arg : spec.args) {
    out.push_back(' ');
    if (arg.find(' ') != std::string::npos || arg.find('\t') != std::string::npos) {
      out += str::quote(arg);
    } else {
      out += arg;
    }
  }
  return out;
}

}  // namespace cova::proc

#endif  // _WIN32
