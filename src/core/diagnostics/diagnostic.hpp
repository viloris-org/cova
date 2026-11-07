// Cova — structured diagnostics (PRD §7.7, §19.3).
//
// Diagnostics are Rust-flavoured: a primary message with a stable code, plus
// optional nested notes/help lines. Every resolver failure in Cova must render
// as a diagnostic that states what was required, what was found, and what is
// missing.
#pragma once

#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"

namespace cova {

enum class Severity { Note, Help, Warning, Error, Bug };

std::string_view to_string(Severity severity);

struct Diagnostic {
  Severity severity = Severity::Error;
  std::string code;      // e.g. "cova::toolchain::unresolved"
  std::string message;   // one-line summary
  std::string detail;    // optional multi-line body
  std::vector<Diagnostic> children;

  // Appends a child diagnostic (rendered indented underneath).
  Diagnostic& add(Diagnostic child);
  // Appends a "  label: value" note line to `detail`.
  Diagnostic& note(std::string label, std::string value);
  // Appends a plain note line.
  Diagnostic& line(std::string text);

  bool is_error() const;
};

Diagnostic make_error(std::string code, std::string message);
Diagnostic make_warning(std::string code, std::string message);
Diagnostic make_note(std::string code, std::string message);
Diagnostic make_help(std::string code, std::string message);

// Converts an Error into an Error diagnostic (preserving context lines).
Diagnostic diagnostic_from_error(const Error& error);

// Collects diagnostics during one command invocation and renders them.
class DiagnosticEngine {
 public:
  DiagnosticEngine() = default;

  void emit(Diagnostic diagnostic);
  void emit(const Error& error);

  bool has_errors() const;
  std::size_t error_count() const;
  std::size_t warning_count() const;

  const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }
  void clear();

  // `color` may be forced on/off; pass false for CI and piped output.
  void render(std::ostream& os, bool color = true) const;
  // "error: 2 errors, 1 warning emitted"
  std::string summary() const;

 private:
  std::vector<Diagnostic> diagnostics_;
};

// Convenience overload for tests.
std::string render_diagnostics(const DiagnosticEngine& engine, bool color = false);

}  // namespace cova
