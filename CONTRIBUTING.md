# Contributing

How ProjectV is developed, and how to work on it. Read the [code of conduct](CODE_OF_CONDUCT.md);
the project is [MIT licensed](LICENSE).

## How development works

ProjectV has one maintainer, and most work is done in pair sessions with Claude Code. The repository
is set up for that: [AGENTS.md](AGENTS.md) holds the build commands and the conventions an agent (or a
person) needs before touching the code, and design happens in written plans before code.

```text
plan (docs/plans/)  ──▶  build in milestones  ──▶  tests + a demo  ──▶  commit to main  ──▶  plan updated
  decisions, D1..          M0, M1, ...               ctest; an example     one commit per       status line,
  open questions           each a commit             that exercises it     milestone            "as built"
```

- **Plans first.** Anything bigger than a fix starts as a plan in [`docs/plans/`](docs/plans/README.md):
  the problem, the decisions (numbered, each with its reason), milestones, open questions. It is
  agreed before code is written, and kept current as work lands: its status line, what was built,
  and where the build differed. [`physics-jolt.md`](docs/plans/physics-jolt.md) is the model.
- **Commits go to `main`.** Large or experimental work gets a branch (`runtime-spine`,
  `component-attachments`, `surface-voxels`). Parallel sessions get their own git worktree, so one
  doesn't trample another's working tree.
- **There is no CI.** The checks below are run locally before committing.
- **Defects found along the way** that are out of scope go in
  [`known-latent-issues.md`](docs/plans/known-latent-issues.md), not into an unrelated commit.

## Before you commit

- [ ] `ctest --preset dev` passes: the unit suite, the `layering` check and the `docs` link check
- [ ] After physics changes: `ctest --preset soak`, and the demos' self-test
      (`cd build/examples/physics_demos && DEMOS_SELFTEST=1 ./physics_demos`)
- [ ] After runtime/sandbox changes: `SANDBOX_SELFTEST=600 ./sandbox`
- [ ] A behaviour change has a test in `tests/unit/` ([tests/README.md](tests/README.md)); a bug
      fix has the test that would have caught it
- [ ] Examples still build and run: they are the integration test, and an API change updates them
- [ ] Docs that describe the behaviour are updated; a new public header has a row in its
      [reference page](docs/reference/README.md) (the `docs` test enforces it)
- [ ] The plan you worked from says what was built

## Commit messages

```text
Area: what changed, as a result rather than an activity

A paragraph or two: what the change does and why, what it decided, what it
deliberately does not do, and which tests and demos cover it. Name the plan
item it delivers.

Co-Authored-By: ...        (when written with an agent)
```

- **The subject leads with the area**: `Physics:`, `Physics demos:`, `Physics M4:`, `Physics plan:`.
- **Say what is true now**, not what was done: "breaking on impact is asked for, not assumed", not
  "changed breaksOnImpact default".
- **One milestone or one fix per commit**, with its tests and demo in the same commit.
- Agent-written commits carry a `Co-Authored-By` trailer.

## Working with an agent

- Point it at [AGENTS.md](AGENTS.md) and the plan for the area. Keep both current: AGENTS.md is the
  agent's only standing knowledge of the codebase.
- Plans live in the repo, never in a session's scratch space or elsewhere on disk.
- One session per worktree. A second session on the same tree works on a branch of its own.

## Outside contributions

Welcome. Open an issue first for anything beyond a small fix, so it can be weighed against the
plans. Pull requests go to `main`; the checklist above applies.

**Bug reports** need: what you did, what happened, what you expected, and the log (the `[WRN]` and
`[ERR]` lines especially). For a physics bug, a recording (`PhysicsSettings::record`,
`PhysicsLog::saveToFile`) reproduces it exactly.

## Where things go

| Change | Read first |
|---|---|
| Any code | [CODING_STYLE.md](CODING_STYLE.md), [Architecture](docs/concepts/architecture.md) (what each module may include) |
| Data that is saved | [Where data goes](docs/concepts/where-data-goes.md) |
| A file format | [docs/reference/formats/](docs/reference/formats/README.md): update the page in the same commit |
| Docs | [Writing docs](docs/contributing/writing-docs.md) |
| Physics | [Physics](docs/concepts/physics.md): determinism rules, and every Jolt input goes through `PhysicsWorld` |

## Citations

Anything built on a paper, an article or someone else's code is cited in [SOURCES.md](SOURCES.md):

```text
### What it is used for
  - [Name of source](link) - Accessed on day/month/year
```
