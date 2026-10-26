"""Where every Markdown file in the repository lives on the docs site, and how a link between two
of them is rewritten.

Shared by the MkDocs hook (mkdocs_hooks.py) and the link checker (check_links.py), so the site
and the check agree on what a link means. Standard library only.

The rule the docs are written to: **a link is a path relative to the file it is in**, exactly as
`cat` and GitHub read it. The site moves some files (the example READMEs live outside docs/), so
the hook rewrites each link to wherever its target ended up. A link to a source file (a header,
a .cpp, a folder with no README) becomes a link to that file on GitHub.
"""

import os
import re

REPO_URL = "https://github.com/ajsuper/Projectv"
BRANCH = "main"

# Markdown outside docs/ that the site carries, as repo-relative globs (dir/*/README.md style).
EXTRA_PAGES = [
    "README.md",
    "CONTRIBUTING.md",
    "CODING_STYLE.md",
    "CODE_OF_CONDUCT.md",
    "SOURCES.md",
    "AGENTS.md",
    "examples/README.md",
    "examples/*/README.md",
    "examples/*/renderers/*/README.md",
    "tests/README.md",
]

ROOT_PREFIX = "project/"   # where root-level files go on the site (docs/README.md is the home page)

LINK = re.compile(r"(!?\[[^\]]*\]\()([^)\s]+)((?:\s+\"[^\"]*\")?\))")
FENCE = re.compile(r"^\s*(```|~~~)")


def repo_root():
    return os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def extra_pages(root):
    """Repo-relative paths of the Markdown files outside docs/ that the site carries."""
    import glob
    found = []
    for pattern in EXTRA_PAGES:
        for path in sorted(glob.glob(os.path.join(root, pattern))):
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            if "/external/" not in rel and rel not in found:
                found.append(rel)
    return found


def site_path(repo_path):
    """The site source path (MkDocs src_uri) a repo file is published at, or None."""
    if repo_path.startswith("docs/"):
        return repo_path[len("docs/"):]
    if "/" not in repo_path:
        return ROOT_PREFIX + repo_path
    return repo_path


def repo_path_of(src_uri):
    """Inverse of site_path."""
    if src_uri.startswith(ROOT_PREFIX):
        return src_uri[len(ROOT_PREFIX):]
    if src_uri.startswith("examples/") or src_uri.startswith("tests/"):
        return src_uri
    return "docs/" + src_uri


def split_target(target):
    path, sep, anchor = target.partition("#")
    return path, (sep + anchor) if sep else ""


def is_external(target):
    return bool(re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", target)) or target.startswith("#")


def resolve(page_repo_path, target):
    """The repo-relative path a link in `page_repo_path` points at (no anchor), or None if it
    leaves the repository."""
    path, _ = split_target(target)
    if not path:
        return None
    if path.startswith("/"):
        joined = path.lstrip("/")
    else:
        joined = os.path.join(os.path.dirname(page_repo_path), path)
    joined = os.path.normpath(joined).replace(os.sep, "/")
    if joined.startswith(".."):
        return None
    return joined


def code_spans(line):
    """[start, end) of every `inline code` span in a line."""
    return [m.span() for m in re.finditer(r"(`+)(?:(?!\1).)*?\1", line)]


def links_in(line):
    """Link matches whose target is not inside inline code. Link text may contain code."""
    spans = code_spans(line)
    for match in LINK.finditer(line):
        at = match.start(2)
        if not any(start <= at < end for start, end in spans):
            yield match


def iter_links(markdown):
    """(line number, target) for every link outside fenced code and inline code."""
    in_fence = False
    for number, line in enumerate(markdown.splitlines(), 1):
        if FENCE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        for match in links_in(line):
            yield number, match.group(2)


def rewrite(markdown, transform):
    """Applies transform(target) -> new target to every link outside code."""
    out = []
    in_fence = False
    for line in markdown.split("\n"):
        if FENCE.match(line):
            in_fence = not in_fence
        elif not in_fence:
            for match in reversed(list(links_in(line))):
                start, end = match.span(2)
                line = line[:start] + transform(match.group(2)) + line[end:]
        out.append(line)
    return "\n".join(out)


def github_url(repo_path, root):
    kind = "tree" if os.path.isdir(os.path.join(root, repo_path)) else "blob"
    return f"{REPO_URL}/{kind}/{BRANCH}/{repo_path}"
