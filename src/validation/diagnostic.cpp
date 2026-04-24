#include "needle/validation/diagnostic.h"
#include <algorithm>

namespace needle {

void Diagnostics::add(Diagnostic d) {
    diagnostics_.push_back(std::move(d));
}

bool Diagnostics::has_errors() const {
    for (const auto& d : diagnostics_) {
        if (d.severity == DiagnosticSeverity::ERROR) return true;
    }
    return false;
}

bool Diagnostics::has_warnings() const {
    for (const auto& d : diagnostics_) {
        if (d.severity == DiagnosticSeverity::WARNING) return true;
    }
    return false;
}

const std::vector<Diagnostic>& Diagnostics::all() const {
    return diagnostics_;
}

std::vector<Diagnostic> Diagnostics::errors() const {
    std::vector<Diagnostic> result;
    for (const auto& d : diagnostics_) {
        if (d.severity == DiagnosticSeverity::ERROR) {
            result.push_back(d);
        }
    }
    return result;
}

void Diagnostics::print(std::ostream& out, bool use_color) const {
    for (const auto& d : diagnostics_) {
        std::string prefix;
        std::string reset;

        if (use_color) {
            switch (d.severity) {
                case DiagnosticSeverity::ERROR:
                    prefix = "\033[31m";  // red
                    break;
                case DiagnosticSeverity::WARNING:
                    prefix = "\033[33m";  // yellow
                    break;
                case DiagnosticSeverity::INFO:
                    prefix = "\033[34m";  // blue
                    break;
            }
            reset = "\033[0m";
        }

        std::string severity_str;
        switch (d.severity) {
            case DiagnosticSeverity::ERROR:   severity_str = "ERROR";   break;
            case DiagnosticSeverity::WARNING: severity_str = "WARNING"; break;
            case DiagnosticSeverity::INFO:    severity_str = "INFO";    break;
        }

        out << prefix << severity_str << reset
            << " [" << d.code << "]";
        if (!d.node_id.empty()) {
            out << " (node: " << d.node_id << ")";
        }
        out << ": " << d.message << "\n";
    }
}

} // namespace needle
