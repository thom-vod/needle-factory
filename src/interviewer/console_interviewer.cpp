#include "needle/interviewer/interviewer.h"
#include <iostream>
#include <string>
#include <cstdlib>
#include <algorithm>

namespace needle {

Result<InterviewAnswer> ConsoleInterviewer::ask(const InterviewQuestion& q) {
    std::cout << "\n" << q.prompt << "\n";

    for (size_t i = 0; i < q.choices.size(); ++i) {
        std::cout << "  [" << (i + 1) << "] " << q.choices[i] << "\n";
    }

    std::cout << "\nEnter choice (1-" << q.choices.size() << ")";
    if (q.choices.size() == 1) {
        std::cout << " [default: 1]";
    }
    std::cout << "\n  (or type feedback text to select the last option with a note)";
    std::cout << "\n> " << std::flush;

    std::string line;
    if (!std::getline(std::cin, line)) {
        return Result<InterviewAnswer>::failure("failed to read from stdin");
    }

    // Trim whitespace
    std::string trimmed = line;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
    trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);

    // Default to first choice on empty input when there's only one option
    if (trimmed.empty() && q.choices.size() == 1) {
        trimmed = "1";
    }

    // Try to parse as a number
    int choice = std::atoi(trimmed.c_str());
    bool is_numeric = !trimmed.empty() &&
        std::all_of(trimmed.begin(), trimmed.end(), ::isdigit);

    InterviewAnswer answer;

    if (is_numeric && choice >= 1 && choice <= static_cast<int>(q.choices.size())) {
        // Numeric choice
        answer.selected_index = choice - 1;
        answer.raw_input = trimmed;
    } else if (trimmed.empty()) {
        return Result<InterviewAnswer>::failure(
            "empty input (enter a number or type feedback)");
    } else {
        // Freeform text — select the last choice (typically the reject/fix edge)
        // and attach the feedback as raw_input
        answer.selected_index = static_cast<int>(q.choices.size()) - 1;
        answer.raw_input = trimmed;
        std::cout << "  → Selected [" << q.choices.size() << "] "
                  << q.choices.back() << " with feedback\n";
    }

    return Result<InterviewAnswer>::success(std::move(answer));
}

} // namespace needle
