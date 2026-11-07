// Cova — structured diagnostics (implementation).
//
// Rendering is Rust-flavoured and deliberately plain: no ANSI escapes are ever
// emitted when `color` is false, so CI logs and piped output stay clean.
#include "core/diagnostics/diagnostic.hpp"

#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"

namespace cova {
namespace {

constexpr std::string_view kReset = "\x1b[0m";
constexpr std::string_view kBold = "\x1b[1m";
constexpr std::string_view kRed = "\x1b[31;1m";
constexpr std::string_view kYellow = "\x1b[33;1m";
constexpr std::string_view kCyan = "\x1b[36;1m";
constexpr std::string_view kBlue = "\x1b[34;1m";
constexpr std::string_view kMagenta = "\x1b[35;1m";

std::string_view severity_color(Severity severity) {
  switch (severity) {
    case Severity::Note:
      return kBlue;
    case Severity::Help:
      return kCyan;
    case Severity::Warning:
      return kYellow;
    case Severity::Error:
      return kRed;
    case Severity::Bug:
      return kMagenta;
  }
  return kRed;
}

void render_one(std::ostream& os, const Diagnostic& diagnostic, int depth, bool color) {
  const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
  const std::string_view label = to_string(diagnostic.severity);

  if (color) {
    os << indent << severity_color(diagnostic.severity) << label << kReset << ": ";
    os << kBold << diagnostic.message << kReset;
  } else {
    os << indent << label << ": " << diagnostic.message;
  }
  if (!diagnostic.code.empty()) {
    if (color) {
      os << " " << kCyan << "[" << diagnostic.code << "]" << kReset;
    } else {
      os << " [" << diagnostic.code << "]";
    }
  }
  os << "\n";

  if (!diagnostic.detail.empty()) {
    std::size_t begin = 0;
    const std::string& detail = diagnostic.detail;
    while (begin <= detail.size()) {
      const std::size_t end = detail.find('\n', begin);
      const std::string_view line =
          end == std::string::npos ? std::string_view(detail).substr(begin)
                                   : std::string_view(detail).substr(begin, end - begin);
      os << indent << "  " << line << "\n";
      if (end == std::string::npos) {
        break;
      }
      begin = end + 1;
    }
  }

  for (const Diagnostic& child : diagnostic.children) {
    render_one(os, child, depth + 1, color);
  }
}

}  // namespace

std::string_view to_string(Severity severity) {
  switch (severity) {
    case Severity::Note:
      return "note";
    case Severity::Help:
      return "help";
    case Severity::Warning:
      return "warning";
    case Severity::Error:
      return "error";
    case Severity::Bug:
      return "bug";
  }
  return "error";
}

// ---------------------------------------------------------------------------
// Diagnostic
// ---------------------------------------------------------------------------

Diagnostic& Diagnostic::add(Diagnostic child) {
  children.push_back(std::move(child));
  return *this;
}

Diagnostic& Diagnostic::note(std::string label, std::string value) {
  if (!detail.empty()) {
    detail.push_back('\n');
  }
  detail += "  ";
  detail += label;
  detail += ": ";
  detail += value;
  return *this;
}

Diagnostic& Diagnostic::line(std::string text) {
  if (!detail.empty()) {
    detail.push_back('\n');
  }
  detail += "  ";
  detail += text;
  return *this;
}

bool Diagnostic::is_error() const {
  return severity == Severity::Error || severity == Severity::Bug;
}

Diagnostic make_error(std::string code, std::string message) {
  Diagnostic diagnostic;
  diagnostic.severity = Severity::Error;
  diagnostic.code = std::move(code);
  diagnostic.message = std::move(message);
  return diagnostic;
}

Diagnostic make_warning(std::string code, std::string message) {
  Diagnostic diagnostic;
  diagnostic.severity = Severity::Warning;
  diagnostic.code = std::move(code);
  diagnostic.message = std::move(message);
  return diagnostic;
}

Diagnostic make_note(std::string code, std::string message) {
  Diagnostic diagnostic;
  diagnostic.severity = Severity::Note;
  diagnostic.code = std::move(code);
  diagnostic.message = std::move(message);
  return diagnostic;
}

Diagnostic make_help(std::string code, std::string message) {
  Diagnostic diagnostic;
  diagnostic.severity = Severity::Help;
  diagnostic.code = std::move(code);
  diagnostic.message = std::move(message);
  return diagnostic;
}

Diagnostic diagnostic_from_error(const Error& error) {
  Diagnostic diagnostic = make_error(error.code, error.message);
  for (const std::string& context_line : error.context) {
    diagnostic.line(context_line);
  }
  return diagnostic;
}

// ---------------------------------------------------------------------------
// DiagnosticEngine
// ---------------------------------------------------------------------------

void DiagnosticEngine::emit(Diagnostic diagnostic) { diagnostics_.push_back(std::move(diagnostic)); }

void DiagnosticEngine::emit(const Error& error) { emit(diagnostic_from_error(error)); }

bool DiagnosticEngine::has_errors() const { return error_count() > 0; }

std::size_t DiagnosticEngine::error_count() const {
  std::size_t count = 0;
  for (const Diagnostic& diagnostic : diagnostics_) {
    if (diagnostic.is_error()) {
      ++count;
    }
  }
  return count;
}

std::size_t DiagnosticEngine::warning_count() const {
  std::size_t count = 0;
  for (const Diagnostic& diagnostic : diagnostics_) {
    if (diagnostic.severity == Severity::Warning) {
      ++count;
    }
  }
  return count;
}

void DiagnosticEngine::clear() { diagnostics_.clear(); }

void DiagnosticEngine::render(std::ostream& os, bool color) const {
  for (const Diagnostic& diagnostic : diagnostics_) {
    render_one(os, diagnostic, 0, color);
  }
}

std::string DiagnosticEngine::summary() const {
  const std::size_t errors = error_count();
  const std::size_t warnings = warning_count();
  std::string out;
  out += std::to_string(errors);
  out += errors == 1 ? " error" : " errors";
  out += ", ";
  out += std::to_string(warnings);
  out += warnings == 1 ? " warning" : " warnings";
  out += " emitted";
  return out;
}

std::string render_diagnostics(const DiagnosticEngine& engine, bool color) {
  std::ostringstream os;
  engine.render(os, color);
  return os.str();
}

}  // namespace cova
