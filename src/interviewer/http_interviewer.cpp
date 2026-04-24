#ifdef NEEDLE_ENABLE_SERVER

#include "http_interviewer.h"
#include <nlohmann/json.hpp>

namespace needle {

Result<InterviewAnswer> HttpInterviewer::ask(const InterviewQuestion& q) {
    std::unique_lock<std::mutex> lock(mutex_);

    // Park the question
    current_question_ = q;
    question_pending_ = true;
    answer_ready_ = false;

    // Wait until an answer is posted
    cv_.wait(lock, [this] { return answer_ready_; });

    question_pending_ = false;

    // Parse the answer JSON
    try {
        nlohmann::json j = nlohmann::json::parse(answer_json_);
        InterviewAnswer answer;
        if (j.count("selected_index")) {
            answer.selected_index = j["selected_index"].get<int>();
        } else {
            answer.selected_index = 0;
        }
        if (j.count("raw_input")) {
            answer.raw_input = j["raw_input"].get<std::string>();
        }
        return Result<InterviewAnswer>::success(std::move(answer));
    } catch (const std::exception& e) {
        // Try to interpret as a simple index
        InterviewAnswer answer;
        answer.selected_index = 0;
        answer.raw_input = answer_json_;
        return Result<InterviewAnswer>::success(std::move(answer));
    }
}

void HttpInterviewer::post_answer(const std::string& answer_json) {
    std::lock_guard<std::mutex> lock(mutex_);
    answer_json_ = answer_json;
    answer_ready_ = true;
    cv_.notify_one();
}

bool HttpInterviewer::has_pending_question() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return question_pending_;
}

InterviewQuestion HttpInterviewer::pending_question() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_question_;
}

} // namespace needle

#endif // NEEDLE_ENABLE_SERVER
