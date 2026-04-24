#include "needle/interviewer/interviewer.h"

namespace needle {

Result<InterviewAnswer> AutoApproveInterviewer::ask(const InterviewQuestion& q) {
    if (q.choices.empty()) {
        return Result<InterviewAnswer>::failure("no choices available");
    }
    InterviewAnswer answer;
    answer.selected_index = 0;
    answer.raw_input = q.choices[0];
    return Result<InterviewAnswer>::success(std::move(answer));
}

} // namespace needle
