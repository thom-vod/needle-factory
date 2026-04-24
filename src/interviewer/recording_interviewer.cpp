#include "needle/interviewer/interviewer.h"

namespace needle {

RecordingInterviewer::RecordingInterviewer(std::shared_ptr<Interviewer> inner)
    : inner_(std::move(inner))
{
}

Result<InterviewAnswer> RecordingInterviewer::ask(const InterviewQuestion& q) {
    auto result = inner_->ask(q);
    if (result.ok()) {
        recording_.push_back(std::make_pair(q, result.value()));
    }
    return result;
}

const std::vector<std::pair<InterviewQuestion, InterviewAnswer>>& RecordingInterviewer::recording() const {
    return recording_;
}

} // namespace needle
