#pragma once

#ifdef NEEDLE_ENABLE_SERVER

#include <string>
#include <mutex>
#include <condition_variable>
#include <map>
#include "needle/interviewer/interviewer.h"

namespace needle {

// HttpInterviewer parks questions in a thread-safe queue with mutex +
// condition_variable. ask() blocks until an answer is posted via the
// HTTP server's /answer endpoint.
class HttpInterviewer : public Interviewer {
public:
    Result<InterviewAnswer> ask(const InterviewQuestion& q) override;

    // Called by the HTTP server when an answer is posted
    void post_answer(const std::string& answer_json);

    // Check if there is a pending question
    bool has_pending_question() const;

    // Get the current pending question (for the HTTP server to expose)
    InterviewQuestion pending_question() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool question_pending_;
    bool answer_ready_;
    InterviewQuestion current_question_;
    std::string answer_json_;

public:
    HttpInterviewer() : question_pending_(false), answer_ready_(false) {}
};

} // namespace needle

#endif // NEEDLE_ENABLE_SERVER
