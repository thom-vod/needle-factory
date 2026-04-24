#pragma once

#include <string>
#include <vector>
#include <map>
#include <nlohmann/json.hpp>
#include "needle/model/result.h"
#include "needle/model/context.h"

namespace needle {

struct Checkpoint {
    std::string timestamp;
    std::string current_node;
    std::vector<std::string> completed_nodes;
    std::map<std::string, int> retry_counters;
    Context context;
    std::string graph_file;
    std::string graph_hash;
    std::string stylesheet_file;
    std::string logs_root;

    nlohmann::json to_json() const;
    static Result<Checkpoint> from_json(const nlohmann::json& j);
};

class CheckpointWriter {
public:
    virtual ~CheckpointWriter() {}
    virtual Result<void> save(const Checkpoint& cp, const std::string& path) = 0;
    virtual Result<Checkpoint> load(const std::string& path) = 0;
};

class JsonCheckpointWriter : public CheckpointWriter {
public:
    Result<void> save(const Checkpoint& cp, const std::string& path) override;
    Result<Checkpoint> load(const std::string& path) override;
};

class InMemoryCheckpointWriter : public CheckpointWriter {
public:
    Result<void> save(const Checkpoint& cp, const std::string& path) override;
    Result<Checkpoint> load(const std::string& path) override;

private:
    std::map<std::string, nlohmann::json> store_;
};

} // namespace needle
