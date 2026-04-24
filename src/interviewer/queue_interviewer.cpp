#include "needle/interviewer/interviewer.h"

namespace needle {

QueueInterviewer::QueueInterviewer(std::vector<InterviewAnswer> answers)
    : answers_(std::move(answers))
    , index_(0)
{
}

Result<InterviewAnswer> QueueInterviewer::ask(const InterviewQuestion& q) {
    (void)q;
    if (index_ >= answers_.size()) {
        return Result<InterviewAnswer>::failure("no more queued answers");
    }
    InterviewAnswer answer = answers_[index_];
    ++index_;
    return Result<InterviewAnswer>::success(std::move(answer));
}

} // namespace needle
