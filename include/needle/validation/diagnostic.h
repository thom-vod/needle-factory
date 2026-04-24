#pragma once

#include <string>
#include <vector>
#include <iostream>

// Windows headers #define ERROR 0 — undefine to avoid collision with our enum
#ifdef ERROR
#undef ERROR
#endif

namespace needle {

enum class DiagnosticSeverity { ERROR, WARNING, INFO };

struct Diagnostic {
    DiagnosticSeverity severity;
    std::string code;       // "E001", "W003", "I001"
    std::string message;
    std::string node_id;    // "" for graph-level diagnostics
};

class Diagnostics {
public:
    void add(Diagnostic d);
    bool has_errors() const;
    bool has_warnings() const;
    const std::vector<Diagnostic>& all() const;
    std::vector<Diagnostic> errors() const;
    void print(std::ostream& out, bool use_color = true) const;

private:
    std::vector<Diagnostic> diagnostics_;
};

} // namespace needle
