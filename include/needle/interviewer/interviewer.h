#pragma once

#include <string>
#include <vector>
#include <memory>
#include <utility>
#include <mutex>
#include <condition_variable>
#include <functional>
#include "needle/model/result.h"

namespace needle {

struct InterviewQuestion {
    std::string prompt;
    std::vector<std::string> choices;  // derived from outgoing edge labels
};

struct InterviewAnswer {
    int selected_index;    // index into choices
    std::string raw_input; // for freeform
};

class Interviewer {
public:
    virtual ~Interviewer() {}
    virtual Result<InterviewAnswer> ask(const InterviewQuestion& q) = 0;
};

class ConsoleInterviewer : public Interviewer {
public:
    Result<InterviewAnswer> ask(const InterviewQuestion& q) override;
};

class AutoApproveInterviewer : public Interviewer {
public:
    Result<InterviewAnswer> ask(const InterviewQuestion& q) override;
};

class QueueInterviewer : public Interviewer {
public:
    explicit QueueInterviewer(std::vector<InterviewAnswer> answers);
    Result<InterviewAnswer> ask(const InterviewQuestion& q) override;

private:
    std::vector<InterviewAnswer> answers_;
    size_t index_;
};

class RecordingInterviewer : public Interviewer {
public:
    explicit RecordingInterviewer(std::shared_ptr<Interviewer> inner);
    Result<InterviewAnswer> ask(const InterviewQuestion& q) override;
    const std::vector<std::pair<InterviewQuestion, InterviewAnswer>>& recording() const;

private:
    std::shared_ptr<Interviewer> inner_;
    std::vector<std::pair<InterviewQuestion, InterviewAnswer>> recording_;
};

} // namespace needle
