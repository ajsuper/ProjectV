# Prototypes

Quick, throwaway experiments to see what an idea looks like before it has a plan. Not examples, not
engine code: they may be slow, single-threaded and untidy, and are deleted or rewritten when the idea
gets a plan in `docs/plans/`. Built with `-DPROJV_BUILD_PROTOTYPES=ON`; each lands in
`build/examples/<name>/` like an example.

| Folder | Target | What |
|---|---|---|
| `fluid/` | `fluid_prototype` | FLIP water on a grid aligned with a 64^3 voxel chunk: a dam to break (E), a trench to dig (X), water to drop (G), reset (T) |
