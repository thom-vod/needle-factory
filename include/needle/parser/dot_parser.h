#pragma once

#include <string>
#include "needle/model/result.h"
#include "needle/parser/dot_ast.h"
#include "needle/parser/dot_lexer.h"

namespace needle {

class DotParser {
public:
    explicit DotParser(const std::string& source);

    Result<AstGraph> parse();
    std::string error_message() const;

private:
    static const size_t MAX_SOURCE_SIZE = 10 * 1024 * 1024;  // 10MB

    Result<AstGraph> parse_graph();
    Result<void> parse_stmt_list(AstGraph& graph);
    Result<void> parse_stmt(AstGraph& graph);
    Result<AstNode> parse_node_stmt(const std::string& first_id);
    Result<AstEdge> parse_edge_stmt(const std::string& first_id);
    Result<std::vector<AstAttribute>> parse_attr_list();
    Result<AstSubgraph> parse_subgraph();
    Result<AstDefaultAttrs> parse_default_attrs(const std::string& target);

    // Subgraph statement list parsing
    Result<void> parse_stmt_list_into(
        std::vector<AstNode>& nodes,
        std::vector<AstEdge>& edges,
        std::vector<AstDefaultAttrs>& defaults,
        std::vector<AstSubgraph>& subgraphs);
    Result<void> parse_stmt_into(
        std::vector<AstNode>& nodes,
        std::vector<AstEdge>& edges,
        std::vector<AstDefaultAttrs>& defaults,
        std::vector<AstSubgraph>& subgraphs);

    Token expect(TokenType type);
    bool match(TokenType type);

    DotLexer lexer_;
    Token current_;
    std::string error_;
};

} // namespace needle
