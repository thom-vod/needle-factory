#include <catch2/catch.hpp>

#ifdef NEEDLE_ENABLE_SERVER

#include "needle/server/model_cache.h"
#include <thread>
#include <atomic>
#include <vector>

using namespace needle;

TEST_CASE("ModelCache: store and retrieve", "[model_cache]") {
    ModelCache cache;
    nlohmann::json models = nlohmann::json::array({"gpt-4o", "gpt-3.5"});

    cache.store("openai", models);

    auto result = cache.get_if_fresh("openai");
    REQUIRE(result.first == true);
    REQUIRE(result.second == models);
}

TEST_CASE("ModelCache: miss on unknown provider", "[model_cache]") {
    ModelCache cache;
    auto result = cache.get_if_fresh("unknown");
    REQUIRE(result.first == false);
}

TEST_CASE("ModelCache: clear removes entries", "[model_cache]") {
    ModelCache cache;
    cache.store("openai", nlohmann::json::array({"gpt-4o"}));
    cache.clear();

    auto result = cache.get_if_fresh("openai");
    REQUIRE(result.first == false);
}

TEST_CASE("ModelCache: concurrent access no crash (M6)", "[model_cache]") {
    ModelCache cache;
    std::atomic<int> stores_done(0);
    std::atomic<int> reads_done(0);

    // Multiple threads reading and writing concurrently
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.push_back(std::thread([&cache, &stores_done]() {
            for (int j = 0; j < 100; ++j) {
                cache.store("provider_" + std::to_string(j % 3),
                           nlohmann::json::array({"model_a", "model_b"}));
                stores_done++;
            }
        }));
        threads.push_back(std::thread([&cache, &reads_done]() {
            for (int j = 0; j < 100; ++j) {
                cache.get_if_fresh("provider_" + std::to_string(j % 3));
                reads_done++;
            }
        }));
    }

    for (auto& t : threads) {
        t.join();
    }

    REQUIRE(stores_done.load() == 400);
    REQUIRE(reads_done.load() == 400);
}

#endif // NEEDLE_ENABLE_SERVER
