#!/usr/bin/env python3
"""Fix common issues in generated DOT files to make them valid attractor format."""
import re
import sys

def fix_dot(text):
    # Remove standalone graph-level attributes
    standalone_attrs = [
        'rankdir', 'fontname', 'fontsize', 'label', 'labelloc',
        'compound', 'newrank', 'bgcolor', 'splines', 'overlap',
        'nodesep', 'ranksep', 'concentrate', 'ordering', 'size',
        'style', 'color', 'fontcolor', 'fillcolor', 'penwidth',
    ]
    pattern = r'^\s*(' + '|'.join(standalone_attrs) + r')\s*=\s*[^;\n]+;?\s*$'
    text = re.sub(pattern, '', text, flags=re.MULTILINE)

    # Remove standalone node/edge default blocks
    text = re.sub(r'^\s*node\s*\[.*?\]\s*;?\s*$', '', text, flags=re.MULTILINE)
    text = re.sub(r'^\s*edge\s*\[.*?\]\s*;?\s*$', '', text, flags=re.MULTILINE)

    # Remove subgraph blocks (keep contents)
    text = re.sub(r'\bsubgraph\s+\w+\s*\{', '', text)

    # Remove style/color/fillcolor/fontcolor from node attributes
    text = re.sub(r',?\s*(style|fillcolor|fontcolor|color|penwidth)\s*=\s*"[^"]*"', '', text)
    text = re.sub(r',?\s*(style|fillcolor|fontcolor|color|penwidth)\s*=\s*\S+', '', text)

    # Clean up empty attribute lists [, ] or [ ,]
    text = re.sub(r'\[\s*,', '[', text)
    text = re.sub(r',\s*\]', ']', text)
    text = re.sub(r'\[\s*\]', '', text)

    # Remove lone closing braces from subgraphs (heuristic)
    lines = text.split('\n')
    result = []
    brace_depth = 0
    for line in lines:
        stripped = line.strip()
        opens = stripped.count('{')
        closes = stripped.count('}')
        brace_depth += opens - closes
        if stripped == '}' and brace_depth < 0:
            brace_depth += 1
            continue
        result.append(line)
    text = '\n'.join(result)

    # Ensure the file ends with a proper closing brace
    text = text.rstrip()
    if not text.endswith('}'):
        text += '\n}\n'

    # Collapse multiple blank lines
    text = re.sub(r'\n{3,}', '\n\n', text)

    return text

if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else 'project.dot'
    try:
        with open(path) as f:
            original = f.read()
        fixed = fix_dot(original)
        if fixed != original:
            with open(path, 'w') as f:
                f.write(fixed)
            print(f'Fixed {path}')
        else:
            print(f'No changes needed for {path}')
    except FileNotFoundError:
        print(f'File not found: {path}', file=sys.stderr)
        sys.exit(1)
