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
    // Per-completed-node hashes captured at the time the node finished.
    // Lets the soft-hash resume check distinguish "operator edited unstarted
    // node" (safe, continue) from "operator edited a node that already ran"
    // (suspicious, warn or block under --strict-graph-hash).
    std::map<std::string, std::string> completed_node_hashes;
    std::map<std::string, std::string> branch_worktrees;
    std::string stylesheet_file;
    std::string logs_root;
    // SPRINT-013 §3.4: content-level hash of the DOT source at run-start.
    // Lets resume detect "DOT file was edited on disk since the run started"
    // and prompt the operator: reload from disk (re-snapshot, accept new
    // content) or continue from the original frozen graph.
    std::string dot_content_hash;

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
