# Writing docs

How the docs are laid out, how to write a page, and how to build and check them.

## Where things go

```text
docs/
  README.md             the front door: a map of everything below
  getting-started/      build it, run it, first program
  concepts/             how the engine works: one idea per page
  guides/               how to do one task, start to finish
  reference/            lookup: one page per module, file formats, glossary
  contributing/         this
  plans/                design notes and delivery logs (see plans/README.md)
  images/
```

| You are writing... | It goes in | Test |
|---|---|---|
| How a part of the engine works | `concepts/` | Someone reads it to understand, not to do |
| Steps to get one thing done | `guides/` | It has a goal and an end |
| A list to look things up in | `reference/` | Someone searches it, nobody reads it top to bottom |
| A format on disk | `reference/formats/` | It describes bytes or JSON keys |
| A design not built yet, or a log of one that was | `plans/` | It will go stale on purpose |
| How to run one example | that example's `README.md` | It lives beside the code |

**One fact, one place.** A header documents its own declarations; the docs say how they fit
together and link to the header. Don't copy a struct into a page. Link it, and show the three
fields that matter.

## Writing a page

The reader should be able to skim it: headings, tables, code. Prose explains what the structure
can't.

**The reader is an experienced C++ developer.** Don't explain C++, CMake, git, GPUs or ECS in
general ("the first build is slow", "run it from the build directory", what ping-pong buffering
is). Say what is specific to ProjectV: its rules, its gotchas, and the design decisions that change
how you use it.

- **Open with one line** saying what the page covers. No preamble.
- **Lead with the picture.** A table, a diagram or a snippet near the top, then the detail.
- **Short paragraphs.** Three lines is a lot. If it's a list, write a list.
- **Rules as bullets**, each with a **bold lead** saying the rule and the rest saying why.
- **Snippets over sentences.** Five lines of real API beat a paragraph about it. Keep them
  compilable in spirit: real names, real signatures.
- **Name things exactly**: `utils::updateScene`, `Scene::geometryPool`, `include/core/time.h`.
- **No walls of text.** If a section runs past a screen of prose, split it or turn it into a table.

### Diagrams

Draw them as text in a ```` ```text ```` block, so they read the same in `cat`, on GitHub and on the
site. Box-drawing characters are fine.

```text
compose.json ──load──▶ Scene ──createTexturesForScene──▶ GPUData ──render──▶ pixels
```

### Links

Every link is a **path relative to the file it is in**, exactly as `cat` and GitHub read it:

```markdown
[the frame loop](../concepts/frame-loop.md)
[Time](../../include/core/time.h)                  <- a header: becomes a GitHub link on the site
[hello-voxel](../../examples/00-hello-voxel/)      <- a folder with a README: becomes that page
```

The site build rewrites them to wherever the target ended up. Never write a site URL.

## Building and checking

| Command | What it does |
|---|---|
| `ls docs/ && cat docs/README.md` | Reads them. Nothing to build. |
| `grep -rn "fixedDelta" docs/ include/` | Finds things. The headers are the API reference. |
| `ctest --preset dev -R docs` | Link check: every link resolves, every anchor exists, every page is reachable from `docs/README.md`, every header is named in its module page. |
| `cmake --build build --target docs` | The web site, plus the Doxygen API reference if Doxygen is installed. Opens at `build/docs/site/index.html`. |
| `cmake --build build --target docs-serve` | The same, served at <http://127.0.0.1:8000> and rebuilt on save. |

The site is [MkDocs](https://www.mkdocs.org/) with the
[Material](https://squidfunk.github.io/mkdocs-material/) theme, installed into a venv in the build
directory on first use (pinned in [`tools/docs/requirements.txt`](../../tools/docs/requirements.txt)).
The pin is deliberate: MkDocs 2.0 drops the plugin system Material depends on. If the site ever
has to move, [Zensical](https://zensical.org/) (Material's successor) reads the same `mkdocs.yml`,
and the Markdown doesn't change either way.
The API reference is [Doxygen](https://www.doxygen.nl/) over `include/`
([`tools/docs/Doxyfile`](../../tools/docs/Doxyfile)). The headers' plain `//` comments are read as
documentation by [`tools/docs/doxygen_filter.py`](../../tools/docs/doxygen_filter.py), so there is
no `///` style to follow. Write the comment above the declaration as usual.

### Adding a page

1. Write it in the right folder.
2. Link it from that folder's `README.md` (the `docs` test fails on a page nothing links to).
3. Add it to `nav` in [`mkdocs.yml`](../../mkdocs.yml).

### Adding a header

Add a row for it in its module's page under [`reference/`](../reference/README.md). The `docs`
test fails on a header no module page names.
