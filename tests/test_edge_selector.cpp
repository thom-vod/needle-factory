#include <catch2/catch.hpp>
#include "needle/engine/edge_selector.h"
#include "needle/model/graph.h"
#include "needle/model/outcome.h"
#include "needle/model/context.h"

using namespace needle;

namespace {

Edge make_edge(const std::string& from, const std::string& to,
               const std::string& label = "", const std::string& condition = "",
               int weight = 0) {
    Edge e;
    e.from = from;
    e.to = to;
    if (!label.empty()) e.attrs.set("label", label);
    if (!condition.empty()) e.attrs.set("condition", condition);
    if (weight != 0) e.attrs.set("weight", std::to_string(weight));
    return e;
}

Outcome make_outcome(StageStatus status = StageStatus::SUCCESS,
                     const std::string& preferred_label = "",
                     const std::vector<std::string>& suggested_next = {}) {
    Outcome o;
    o.status = status;
    o.preferred_label = preferred_label;
    o.suggested_next = suggested_next;
    return o;
}

} // anonymous namespace

TEST_CASE("EdgeSelector: empty candidates returns failure", "[edge_selector]") {
    EdgeSelector selector;
    std::vector<const Edge*> candidates;
    Outcome outcome = make_outcome();
    Context ctx;

    auto result = selector.select(candidates, outcome, ctx);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error() == "no candidate edges");
}

TEST_CASE("EdgeSelector: single candidate returns it directly", "[edge_selector]") {
    EdgeSelector selector;
    Edge e = make_edge("A", "B");
    std::vector<const Edge*> candidates = {&e};
    Outcome outcome = make_outcome();
    Context ctx;

    auto result = selector.select(candidates, outcome, ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value()->to == "B");
}

TEST_CASE("EdgeSelector: Step 1 - condition filtering", "[edge_selector]") {
    EdgeSelector selector;
    Edge e1 = make_edge("A", "B", "", "outcome=SUCCESS");
    Edge e2 = make_edge("A", "C", "", "outcome=FAILURE");
    std::vector<const Edge*> candidates = {&e1, &e2};

    SECTION("SUCCESS outcome selects matching condition") {
        Outcome outcome = make_outcome(StageStatus::SUCCESS);
        Context ctx;

        auto result = selector.select(candidates, outcome, ctx);
        REQUIRE(result.ok());
        REQUIRE(result.value()->to == "B");
    }

    SECTION("FAILURE outcome selects other condition") {
        Outcome outcome = make_outcome(StageStatus::FAILURE);
        Context ctx;

        auto result = selector.select(candidates, outcome, ctx);
        REQUIRE(result.ok());
        REQUIRE(result.value()->to == "C");
    }
}

TEST_CASE("EdgeSelector: Step 1 - unconditional edges as fallback", "[edge_selector]") {
    EdgeSelector selector;
    Edge e1 = make_edge("A", "B", "", "outcome=FAILURE");
    Edge e2 = make_edge("A", "C"); // unconditional
    std::vector<const Edge*> candidates = {&e1, &e2};
    Outcome outcome = make_outcome(StageStatus::SUCCESS);
    Context ctx;

    auto result = selector.select(candidates, outcome, ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value()->to == "C");
}

TEST_CASE("EdgeSelector: Step 1 - context condition", "[edge_selector]") {
    EdgeSelector selector;
    Edge e1 = make_edge("A", "B", "", "context.mode=fast");
    Edge e2 = make_edge("A", "C", "", "context.mode=slow");
    std::vector<const Edge*> candidates = {&e1, &e2};
    Outcome outcome = make_outcome();
    Context ctx;
    ctx.set("mode", "fast");

    auto result = selector.select(candidates, outcome, ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value()->to == "B");
}

TEST_CASE("EdgeSelector: Step 2 - preferred label match", "[edge_selector]") {
    EdgeSelector selector;
    Edge e1 = make_edge("A", "B", "yes");
    Edge e2 = make_edge("A", "C", "no");
    std::vector<const Edge*> candidates = {&e1, &e2};
    Outcome outcome = make_outcome(StageStatus::SUCCESS, "no");
    Context ctx;

    auto result = selector.select(candidates, outcome, ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value()->to == "C");
}

TEST_CASE("EdgeSelector: Step 3 - suggested next match", "[edge_selector]") {
    EdgeSelector selector;
    Edge e1 = make_edge("A", "B");
    Edge e2 = make_edge("A", "C");
    Edge e3 = make_edge("A", "D");
    std::vector<const Edge*> candidates = {&e1, &e2, &e3};
    Outcome outcome = make_outcome(StageStatus::SUCCESS, "", {"D", "B"});
    Context ctx;

    auto result = selector.select(candidates, outcome, ctx);
    REQUIRE(result.ok());
    // D should be found first in suggested_next
    REQUIRE(result.value()->to == "D");
}

TEST_CASE("EdgeSelector: Step 4 - highest weight", "[edge_selector]") {
    EdgeSelector selector;
    Edge e1 = make_edge("A", "B", "", "", 1);
    Edge e2 = make_edge("A", "C", "", "", 5);
    Edge e3 = make_edge("A", "D", "", "", 3);
    std::vector<const Edge*> candidates = {&e1, &e2, &e3};
    Outcome outcome = make_outcome();
    Context ctx;

    auto result = selector.select(candidates, outcome, ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value()->to == "C");
}

TEST_CASE("EdgeSelector: Step 5 - lexical tiebreak", "[edge_selector]") {
    EdgeSelector selector;
    Edge e1 = make_edge("A", "C");
    Edge e2 = make_edge("A", "B");
    Edge e3 = make_edge("A", "D");
    std::vector<const Edge*> candidates = {&e1, &e2, &e3};
    Outcome outcome = make_outcome();
    Context ctx;

    auto result = selector.select(candidates, outcome, ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value()->to == "B");
}

TEST_CASE("EdgeSelector: conditional edges preferred over unconditional", "[edge_selector]") {
    EdgeSelector selector;
    Edge e_cond = make_edge("A", "B", "", "outcome=SUCCESS");
    Edge e_uncond = make_edge("A", "C");
    std::vector<const Edge*> candidates = {&e_cond, &e_uncond};
    Outcome outcome = make_outcome(StageStatus::SUCCESS);
    Context ctx;

    auto result = selector.select(candidates, outcome, ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value()->to == "B");
}

TEST_CASE("EdgeSelector: no conditions match falls back to unconditional", "[edge_selector]") {
    EdgeSelector selector;
    Edge e1 = make_edge("A", "B", "", "outcome=FAILURE");
    Edge e2 = make_edge("A", "C"); // unconditional fallback
    std::vector<const Edge*> candidates = {&e1, &e2};
    Outcome outcome = make_outcome(StageStatus::SUCCESS);
    Context ctx;

    auto result = selector.select(candidates, outcome, ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value()->to == "C");
}

// M2: single conditional edge whose condition doesn't match must return failure
TEST_CASE("EdgeSelector: single conditional edge that doesn't match returns failure", "[edge_selector]") {
    EdgeSelector selector;
    Edge e = make_edge("A", "B", "", "outcome=FAILURE");
    std::vector<const Edge*> candidates = {&e};
    Outcome outcome = make_outcome(StageStatus::SUCCESS);
    Context ctx;

    auto result = selector.select(candidates, outcome, ctx);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error() == "no edges match conditions");
}

// M2 regression: single unconditional edge still works
TEST_CASE("EdgeSelector: single unconditional edge still selected after M2 fix", "[edge_selector]") {
    EdgeSelector selector;
    Edge e = make_edge("A", "B");
    std::vector<const Edge*> candidates = {&e};
    Outcome outcome = make_outcome();
    Context ctx;

    auto result = selector.select(candidates, outcome, ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value()->to == "B");
}

// M4: weight ties fall through to lexical tiebreak
TEST_CASE("EdgeSelector: weight tie falls through to lexical tiebreak", "[edge_selector]") {
    EdgeSelector selector;
    Edge e1 = make_edge("A", "Z", "", "", 5);
    Edge e2 = make_edge("A", "B", "", "", 5);
    Edge e3 = make_edge("A", "M", "", "", 3);
    std::vector<const Edge*> candidates = {&e1, &e2, &e3};
    Outcome outcome = make_outcome();
    Context ctx;

    auto result = selector.select(candidates, outcome, ctx);
    REQUIRE(result.ok());
    // Two edges tie at weight 5, should fall through to lexical: B < Z
    REQUIRE(result.value()->to == "B");
}
