"""MkDocs hooks for the ProjectV docs site (see mkdocs.yml).

1. Pages from outside docs/ -- the example READMEs, the root README, CONTRIBUTING -- are added
   to the site, so each one is written once, next to what it describes.
2. Every relative link is rewritten to where its target ended up on the site; links to source
   files become GitHub links. The Markdown on disk keeps plain repo-relative paths, which is what
   `cat` and GitHub read.
3. The Doxygen API reference (build/docs/api/html, made by `cmake --build build --target docs`)
   is copied in at api/. Without it a placeholder page says how to build it.
"""

import os
import sys

from mkdocs.structure.files import File

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import repo_links  # noqa: E402

ROOT = repo_links.repo_root()
API_HTML = os.environ.get("PROJV_API_HTML", os.path.join(ROOT, "build", "docs", "api", "html"))

PLACEHOLDER = """<!doctype html><meta charset="utf-8"><title>API reference not built</title>
<body style="font-family:sans-serif;max-width:40em;margin:4em auto;line-height:1.5">
<h1>API reference not built</h1>
<p>The API reference is generated from the headers in <code>include/</code> by Doxygen.</p>
<pre>sudo dnf install doxygen graphviz
cmake --build build --target docs</pre>
<p>Until then, the headers themselves are the reference: every type and function is documented
where it is declared.</p></body>
"""


def on_files(files, config):
    for repo_path in repo_links.extra_pages(ROOT):
        files.append(File.generated(config, repo_links.site_path(repo_path),
                                    abs_src_path=os.path.join(ROOT, repo_path)))

    if os.path.isfile(os.path.join(API_HTML, "index.html")):
        for directory, _, names in os.walk(API_HTML):
            for name in names:
                absolute = os.path.join(directory, name)
                relative = os.path.relpath(absolute, API_HTML).replace(os.sep, "/")
                files.append(File.generated(config, "api/" + relative, abs_src_path=absolute))
    else:
        files.append(File.generated(config, "api/index.html", content=PLACEHOLDER))
    return files


def on_page_markdown(markdown, page, config, files):
    page_repo = repo_links.repo_path_of(page.file.src_uri)
    page_dir = os.path.dirname(page.file.src_uri)

    def transform(target):
        if repo_links.is_external(target):
            return target
        path, anchor = repo_links.split_target(target)
        resolved = repo_links.resolve(page_repo, target)
        if resolved is None:
            return target
        # The API reference: docs link to it as ../api/index.html (or the like) from wherever.
        if resolved.startswith("docs/api/") or resolved == "docs/api":
            uri = resolved[len("docs/"):]
            return os.path.relpath(uri, page_dir or ".").replace(os.sep, "/") + anchor
        candidates = [resolved]
        if not resolved.endswith(".md"):
            candidates.append(resolved.rstrip("/") + "/README.md")
        for candidate in candidates:
            uri = repo_links.site_path(candidate)
            if uri and files.get_file_from_path(uri):
                return os.path.relpath(uri, page_dir or ".").replace(os.sep, "/") + anchor
        if os.path.exists(os.path.join(ROOT, resolved)):
            return repo_links.github_url(resolved, ROOT) + anchor
        return target   # broken: left for MkDocs to report

    return repo_links.rewrite(markdown, transform)
