#pragma once

#include <string>
#include <vector>
#include <iostream>

namespace needle {

// Values are mixed-case (Error/Warning/Info) rather than UPPERCASE because
// `ERROR` is a macro in Windows wingdi.h, which is transitively pulled in
// via httplib.h/winsock2.h and would clobber the enumerator name.
enum class DiagnosticSeverity { Error, Warning, Info };

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
