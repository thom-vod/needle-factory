#include "needle/util/graph_serializer.h"
#include "needle/platform/platform.h"

#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define popen _popen
#define pclose _pclose
#else
#include <unistd.h>
#endif

namespace needle {

std::string html_escape(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += c;        break;
        }
    }
    return out;
}

namespace {

// Escape a string for DOT attribute values (double-quoted context)
std::string dot_escape(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

const char* shape_for_type(NodeType type) {
    switch (type) {
        case NodeType::START:        return "circle";
        case NodeType::EXIT:         return "doublecircle";
        case NodeType::CONDITIONAL:  return "diamond";
        case NodeType::PARALLEL:     return "doubleoctagon";
        case NodeType::FAN_IN:       return "doubleoctagon";
        case NodeType::CODERGEN:     return "box";
        case NodeType::LLMKIT:       return "box";
        case NodeType::WAIT_HUMAN:   return "house";
        case NodeType::TOOL:         return "component";
        case NodeType::MANAGER_LOOP: return "box3d";
    }
    return "box";
}

const char* fill_for_type(NodeType type) {
    switch (type) {
        case NodeType::START:        return "#e0e0e0";
        case NodeType::EXIT:         return "#e0e0e0";
        case NodeType::WAIT_HUMAN:   return "#fff3cd";
        case NodeType::CONDITIONAL:  return "#d1ecf1";
        case NodeType::PARALLEL:     return "#d4edda";
        case NodeType::FAN_IN:       return "#d4edda";
        default:                     return "#f0f0f0";
    }
}

// RAII temp file that deletes on destruction
struct TempFile {
    std::string path;
    bool valid_;

    TempFile() : valid_(false) {
        path = platform::make_temp_file("ndl");
        valid_ = !path.empty();
    }
    ~TempFile() {
        if (valid_) {
            platform::remove_file(path);
        }
    }
    bool valid() const { return valid_; }

    // Non-copyable
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

// Post-process SVG to inject ndl-node-{id} classes.
// Graphviz SVG output uses <title>node_id</title> inside <g> elements.
// We find each <title>X</title> and add class="ndl-node-X" to the parent <g>.
std::string inject_node_classes(const std::string& svg,
                                const std::vector<Node>& nodes) {
    std::string result = svg;
    for (const auto& node : nodes) {
        // Find <title>node_id</title>
        std::string title_tag = "<title>" + node.id + "</title>";
        std::string::size_type title_pos = result.find(title_tag);
        if (title_pos == std::string::npos) continue;

        // Walk backwards from title_pos to find the opening <g
        std::string::size_type g_pos = result.rfind("<g", title_pos);
        if (g_pos == std::string::npos) continue;

        // Find the closing > of the <g tag
        std::string::size_type g_end = result.find('>', g_pos);
        if (g_end == std::string::npos || g_end > title_pos) continue;

        // Check if there's already a class attribute
        std::string g_tag = result.substr(g_pos, g_end - g_pos);
        std::string class_attr = " class=\"ndl-node-" + node.id + "\"";

        // Insert class attribute before the closing >
        result.insert(g_end, class_attr);
    }
    return result;
}

} // anonymous namespace

std::string graph_to_dot(const Graph& graph) {
    std::ostringstream out;

    std::string name = graph.name().empty() ? "pipeline" : graph.name();
    out << "digraph \"" << dot_escape(name) << "\" {\n";
    out << "    rankdir=TB;\n";
    out << "    bgcolor=transparent;\n";
    out << "    node [fontname=\"sans-serif\", fontsize=11, style=\"rounded,filled\"];\n";
    out << "    edge [fontname=\"sans-serif\", fontsize=9];\n";
    out << "\n";

    for (const auto& node : graph.nodes()) {
        out << "    \"" << dot_escape(node.id) << "\" [";
        out << "label=\"" << dot_escape(node.label()) << "\"";
        out << ", shape=" << shape_for_type(node.type);
        out << ", fillcolor=\"" << fill_for_type(node.type) << "\"";
        out << "];\n";
    }

    out << "\n";

    for (const auto& edge : graph.edges()) {
        out << "    \"" << dot_escape(edge.from) << "\" -> \""
            << dot_escape(edge.to) << "\"";
        std::string lbl = edge.label();
        if (!lbl.empty()) {
            out << " [label=\"" << dot_escape(lbl) << "\"]";
        }
        out << ";\n";
    }

    out << "}\n";
    return out.str();
}

std::string normalize_display_shapes(const std::string& dot_source) {
    static const std::regex edge_re("->");
    // handler/type = interactive (quoted or bare).
    static const std::regex interactive_re(
        "(handler|type)\\s*=\\s*\"?interactive\"?", std::regex::icase);
    // `shape` used as a real attribute key (preceded by start/space/comma/'[').
    static const std::regex shape_re("(^|[\\s,\\[])shape\\s*=", std::regex::icase);

    std::istringstream in(dot_source);
    std::string line;
    std::ostringstream out;
    bool first = true;
    while (std::getline(in, line)) {
        if (!first) out << "\n";
        first = false;

        std::string transformed = line;
        size_t lb = line.find('[');
        if (lb != std::string::npos) {
            // Head token before '[' (trimmed). Skip edges and graph/node/edge
            // default-attribute statements.
            size_t h = line.find_first_not_of(" \t");
            std::string head = (h == std::string::npos) ? "" : line.substr(h, lb - h);
            // strip trailing whitespace from head
            size_t he = head.find_last_not_of(" \t");
            head = (he == std::string::npos) ? "" : head.substr(0, he + 1);

            bool is_edge = std::regex_search(line.substr(0, lb), edge_re);
            bool is_default = (head == "graph" || head == "node" || head == "edge");
            size_t rb = line.rfind(']');
            if (!is_edge && !is_default && rb != std::string::npos && rb > lb) {
                std::string body = line.substr(lb + 1, rb - lb - 1);
                if (std::regex_search(body, interactive_re) &&
                    !std::regex_search(body, shape_re)) {
                    transformed = line.substr(0, lb + 1) + "shape=\"parallelogram\", " +
                                  line.substr(lb + 1);
                }
            }
        }
        out << transformed;
    }
    if (!dot_source.empty() && dot_source.back() == '\n') out << "\n";
    return out.str();
}

std::string dot_to_svg(const std::string& dot_source) {
    if (dot_source.empty()) return "";

    // Check if dot is available
    if (!platform::command_exists("dot")) return "";

    const std::string rendered_dot = normalize_display_shapes(dot_source);

    // Write DOT to temp file
    TempFile tmp;
    if (!tmp.valid()) return "";

    {
        std::ofstream f(tmp.path, std::ios::binary);
        if (!f.is_open()) return "";
        f.write(rendered_dot.c_str(), static_cast<std::streamsize>(rendered_dot.size()));
    }

    // Invoke dot -Tsvg
#ifdef _WIN32
    std::string cmd = "dot -Tsvg \"" + tmp.path + "\" 2>nul";
#else
    std::string cmd = "dot -Tsvg " + tmp.path + " 2>/dev/null";
#endif
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";

    std::string svg;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        svg += buf;
    }

    int status = pclose(pipe);
    if (status != 0) return "";

    return svg;
}

std::string graph_to_svg(const Graph& graph) {
    std::string dot = graph_to_dot(graph);
    std::string svg = dot_to_svg(dot);
    if (svg.empty()) return "";
    return inject_node_classes(svg, graph.nodes());
}

} // namespace needle
