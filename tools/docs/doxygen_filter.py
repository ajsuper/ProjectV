#!/usr/bin/env python3
"""Doxygen INPUT_FILTER: reads ProjectV's `//` comments as documentation.

The headers document every declaration in plain `//` blocks rather than `///`. Doxygen ignores
those, so without this the API reference would be signatures only. Converting the headers would
churn every file and change the house style; this does the conversion as Doxygen reads them, and
nothing on disk changes.

    // A comment block directly above a declaration     ->  /// ... (documents it)
    int x = 0;   // a comment after a member            ->  ///< ... (documents x)
    // ---- Section ----  (followed by a blank line)      ->  left alone
    // A block directly above the first namespace       ->  the file's own description
    comments inside function bodies                     ->  left alone

Usage (from the Doxyfile): INPUT_FILTER = "python3 tools/docs/doxygen_filter.py"
"""

import os
import re
import sys

FILE_NAME = ""

LINE_COMMENT = re.compile(r"^(\s*)//(?!/)\s?(.*)$")
TRAILING = re.compile(r"^(.*?[;,{])(\s*)//(?![/<])\s?(.*)$")
SEPARATOR = re.compile(r"^\s*(?:-{2,}|={2,}).*$|^\s*$")
SCOPE_OPEN = re.compile(r"^\s*(?:template\s*<.*>\s*)?(?:namespace|struct|class|union|enum|extern\s+\"C\")\b")


def strip_strings(code):
    return re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', '""', code)


def filter_lines(lines):
    out = []
    # A stack of brace kinds: True for a scope (namespace/class/struct/enum), False for a body.
    braces = []
    pending_scope = False          # a scope keyword seen, its `{` not yet
    file_comment_done = False
    in_block_comment = False
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]

        if in_block_comment:
            out.append(line)
            if "*/" in line:
                in_block_comment = False
            i += 1
            continue

        in_body = any(not kind for kind in braces)
        m = LINE_COMMENT.match(line)
        if m and not in_body:
            # Gather the whole block of consecutive // lines.
            block = []
            j = i
            while j < n and LINE_COMMENT.match(lines[j]):
                block.append(LINE_COMMENT.match(lines[j]))
                j += 1
            following = lines[j] if j < n else ""
            attached = following.strip() != "" and not following.lstrip().startswith("#")
            if not file_comment_done and following.lstrip().startswith("namespace"):
                # One line out per line in, so Doxygen's line numbers stay the source's.
                texts = [b.group(2) for b in block]
                texts[0] = "/** @file " + FILE_NAME + " " + texts[0]
                texts[-1] = texts[-1] + " */"
                out.extend(texts)
                file_comment_done = True
            elif attached:
                # A section rule at the head of a block ("// ---- Settings ----") titles the
                # members below it; it is not part of the next one's description.
                k = 0
                while k < len(block) and SEPARATOR.match(block[k].group(2)):
                    out.append(lines[i + k])
                    k += 1
                out.extend(b.group(1) + "/// " + b.group(2) for b in block[k:])
            else:
                out.extend(lines[i:j])
            i = j
            continue

        if not in_body:
            t = TRAILING.match(line)
            if t and "//" not in strip_strings(t.group(1)):
                line = t.group(1) + t.group(2) + "///< " + t.group(3)

        code = line.split("//", 1)[0]
        if "/*" in code and "*/" not in code[code.index("/*"):]:
            in_block_comment = True
        code = strip_strings(code)
        if SCOPE_OPEN.match(code) and not code.rstrip().endswith(";"):
            pending_scope = True
        for ch in code:
            if ch == "{":
                braces.append(pending_scope)
                pending_scope = False
            elif ch == "}":
                if braces:
                    braces.pop()
        if code.rstrip().endswith(";"):
            pending_scope = False
        out.append(line)
        i += 1
    return out


def main():
    global FILE_NAME
    FILE_NAME = os.path.basename(sys.argv[1])
    with open(sys.argv[1], encoding="utf-8", errors="replace") as f:
        lines = f.read().split("\n")
    sys.stdout.write("\n".join(filter_lines(lines)))


if __name__ == "__main__":
    main()
