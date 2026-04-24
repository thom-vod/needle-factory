#include "needle/parser/dot_parser.h"
#include <sstream>

namespace needle {

DotParser::DotParser(const std::string& source)
    : lexer_(source), current_(), error_() {
    current_ = lexer_.next_token();
}

std::string DotParser::error_message() const {
    return error_;
}

Token DotParser::expect(TokenType type) {
    Token tok = current_;
    if (tok.type != type) {
        std::ostringstream oss;
        oss << "expected token type " << static_cast<int>(type)
            << " but got " << static_cast<int>(tok.type)
            << " ('" << tok.value << "')"
            << " at line " << tok.line << ", column " << tok.column;
        error_ = oss.str();
        return Token{TokenType::ERROR, error_, tok.line, tok.column};
    }
    current_ = lexer_.next_token();
    return tok;
}

bool DotParser::match(TokenType type) {
    if (current_.type == type) {
        current_ = lexer_.next_token();
        return true;
    }
    return false;
}

Result<AstGraph> DotParser::parse() {
    return parse_graph();
}

Result<AstGraph> DotParser::parse_graph() {
    AstGraph graph;
    graph.strict = false;

    // Optional "strict"
    if (current_.type == TokenType::STRICT) {
        graph.strict = true;
        current_ = lexer_.next_token();
    }

    // "digraph" or "graph"
    if (current_.type != TokenType::DIGRAPH && current_.type != TokenType::GRAPH) {
        std::ostringstream oss;
        oss << "expected 'digraph' or 'graph' at line " << current_.line
            << ", column " << current_.column;
        return Result<AstGraph>::failure(oss.str());
    }
    current_ = lexer_.next_token();

    // Optional graph name
    if (current_.type == TokenType::IDENTIFIER ||
        current_.type == TokenType::QUOTED_STRING) {
        graph.id = current_.value;
        current_ = lexer_.next_token();
    }

    // Opening brace
    Token lbrace = expect(TokenType::LBRACE);
    if (lbrace.type == TokenType::ERROR) {
        return Result<AstGraph>::failure(error_);
    }

    // Statement list
    auto stmt_result = parse_stmt_list(graph);
    if (!stmt_result.ok()) {
        return Result<AstGraph>::failure(stmt_result.error());
    }

    // Closing brace
    Token rbrace = expect(TokenType::RBRACE);
    if (rbrace.type == TokenType::ERROR) {
        return Result<AstGraph>::failure(error_);
    }

    return Result<AstGraph>::success(std::move(graph));
}

Result<void> DotParser::parse_stmt_list(AstGraph& graph) {
    return parse_stmt_list_into(graph.nodes, graph.edges, graph.defaults, graph.subgraphs);
}

Result<void> DotParser::parse_stmt_list_into(
    std::vector<AstNode>& nodes,
    std::vector<AstEdge>& edges,
    std::vector<AstDefaultAttrs>& defaults,
    std::vector<AstSubgraph>& subgraphs) {

    while (current_.type != TokenType::RBRACE &&
           current_.type != TokenType::END_OF_FILE) {
        auto result = parse_stmt_into(nodes, edges, defaults, subgraphs);
        if (!result.ok()) {
            return result;
        }
        // Optional semicolon
        match(TokenType::SEMICOLON);
    }
    return Result<void>::success();
}

Result<void> DotParser::parse_stmt(AstGraph& graph) {
    return parse_stmt_into(graph.nodes, graph.edges, graph.defaults, graph.subgraphs);
}

Result<void> DotParser::parse_stmt_into(
    std::vector<AstNode>& nodes,
    std::vector<AstEdge>& edges,
    std::vector<AstDefaultAttrs>& defaults,
    std::vector<AstSubgraph>& subgraphs) {

    // Default attributes: node [...], edge [...], graph [...]
    if ((current_.type == TokenType::NODE ||
         current_.type == TokenType::EDGE ||
         current_.type == TokenType::GRAPH) &&
        lexer_.peek().type == TokenType::LBRACKET) {
        std::string target;
        if (current_.type == TokenType::NODE) target = "node";
        else if (current_.type == TokenType::EDGE) target = "edge";
        else target = "graph";
        current_ = lexer_.next_token();
        auto result = parse_default_attrs(target);
        if (!result.ok()) {
            return Result<void>::failure(result.error());
        }
        defaults.push_back(std::move(result.value()));
        return Result<void>::success();
    }

    // Subgraph
    if (current_.type == TokenType::SUBGRAPH ||
        (current_.type == TokenType::LBRACE)) {
        auto result = parse_subgraph();
        if (!result.ok()) {
            return Result<void>::failure(result.error());
        }
        subgraphs.push_back(std::move(result.value()));
        return Result<void>::success();
    }

    // Must be an identifier (node or edge statement)
    if (current_.type != TokenType::IDENTIFIER &&
        current_.type != TokenType::QUOTED_STRING &&
        current_.type != TokenType::NUMBER) {
        std::ostringstream oss;
        oss << "unexpected token '" << current_.value
            << "' at line " << current_.line
            << ", column " << current_.column;
        return Result<void>::failure(oss.str());
    }

    std::string first_id = current_.value;
    current_ = lexer_.next_token();

    // Check if this is an edge statement (followed by ->)
    if (current_.type == TokenType::ARROW) {
        auto result = parse_edge_stmt(first_id);
        if (!result.ok()) {
            return Result<void>::failure(result.error());
        }
        edges.push_back(std::move(result.value()));
        return Result<void>::success();
    }

    // It's a node statement (possibly with attributes)
    auto result = parse_node_stmt(first_id);
    if (!result.ok()) {
        return Result<void>::failure(result.error());
    }
    nodes.push_back(std::move(result.value()));
    return Result<void>::success();
}

Result<AstNode> DotParser::parse_node_stmt(const std::string& first_id) {
    AstNode node;
    node.id = first_id;

    if (current_.type == TokenType::LBRACKET) {
        auto attrs = parse_attr_list();
        if (!attrs.ok()) {
            return Result<AstNode>::failure(attrs.error());
        }
        node.attrs = std::move(attrs.value());
    }

    return Result<AstNode>::success(std::move(node));
}

Result<AstEdge> DotParser::parse_edge_stmt(const std::string& first_id) {
    AstEdge edge;
    edge.node_chain.push_back(first_id);

    while (current_.type == TokenType::ARROW) {
        current_ = lexer_.next_token(); // consume ->

        if (current_.type != TokenType::IDENTIFIER &&
            current_.type != TokenType::QUOTED_STRING &&
            current_.type != TokenType::NUMBER) {
            std::ostringstream oss;
            oss << "expected node identifier after '->' at line "
                << current_.line << ", column " << current_.column;
            return Result<AstEdge>::failure(oss.str());
        }

        edge.node_chain.push_back(current_.value);
        current_ = lexer_.next_token();
    }

    if (current_.type == TokenType::LBRACKET) {
        auto attrs = parse_attr_list();
        if (!attrs.ok()) {
            return Result<AstEdge>::failure(attrs.error());
        }
        edge.attrs = std::move(attrs.value());
    }

    return Result<AstEdge>::success(std::move(edge));
}

Result<std::vector<AstAttribute>> DotParser::parse_attr_list() {
    std::vector<AstAttribute> attrs;

    Token lb = expect(TokenType::LBRACKET);
    if (lb.type == TokenType::ERROR) {
        return Result<std::vector<AstAttribute>>::failure(error_);
    }

    while (current_.type != TokenType::RBRACKET &&
           current_.type != TokenType::END_OF_FILE) {

        if (current_.type != TokenType::IDENTIFIER &&
            current_.type != TokenType::QUOTED_STRING) {
            std::ostringstream oss;
            oss << "expected attribute key at line " << current_.line
                << ", column " << current_.column;
            return Result<std::vector<AstAttribute>>::failure(oss.str());
        }

        AstAttribute attr;
        attr.key = current_.value;
        current_ = lexer_.next_token();

        Token eq = expect(TokenType::EQUALS);
        if (eq.type == TokenType::ERROR) {
            return Result<std::vector<AstAttribute>>::failure(error_);
        }

        // Value can be identifier, quoted string, HTML string, or number
        if (current_.type == TokenType::IDENTIFIER ||
            current_.type == TokenType::QUOTED_STRING ||
            current_.type == TokenType::HTML_STRING ||
            current_.type == TokenType::NUMBER) {
            attr.value = current_.value;
            current_ = lexer_.next_token();
        } else {
            std::ostringstream oss;
            oss << "expected attribute value at line " << current_.line
                << ", column " << current_.column;
            return Result<std::vector<AstAttribute>>::failure(oss.str());
        }

        attrs.push_back(std::move(attr));

        // Optional comma or semicolon separator
        if (current_.type == TokenType::COMMA || current_.type == TokenType::SEMICOLON) {
            current_ = lexer_.next_token();
        }
    }

    Token rb = expect(TokenType::RBRACKET);
    if (rb.type == TokenType::ERROR) {
        return Result<std::vector<AstAttribute>>::failure(error_);
    }

    return Result<std::vector<AstAttribute>>::success(std::move(attrs));
}

Result<AstSubgraph> DotParser::parse_subgraph() {
    AstSubgraph sub;

    if (current_.type == TokenType::SUBGRAPH) {
        current_ = lexer_.next_token();
        // Optional subgraph name
        if (current_.type == TokenType::IDENTIFIER ||
            current_.type == TokenType::QUOTED_STRING) {
            sub.id = current_.value;
            current_ = lexer_.next_token();
        }
    }

    Token lb = expect(TokenType::LBRACE);
    if (lb.type == TokenType::ERROR) {
        return Result<AstSubgraph>::failure(error_);
    }

    auto result = parse_stmt_list_into(sub.nodes, sub.edges, sub.defaults, sub.subgraphs);
    if (!result.ok()) {
        return Result<AstSubgraph>::failure(result.error());
    }

    Token rb = expect(TokenType::RBRACE);
    if (rb.type == TokenType::ERROR) {
        return Result<AstSubgraph>::failure(error_);
    }

    return Result<AstSubgraph>::success(std::move(sub));
}

Result<AstDefaultAttrs> DotParser::parse_default_attrs(const std::string& target) {
    AstDefaultAttrs def;
    def.target = target;

    auto attrs = parse_attr_list();
    if (!attrs.ok()) {
        return Result<AstDefaultAttrs>::failure(attrs.error());
    }
    def.attrs = std::move(attrs.value());

    return Result<AstDefaultAttrs>::success(std::move(def));
}

} // namespace needle
