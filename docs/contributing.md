# Contributing

## Style

- C++23, 4-space indentation, small free functions in anonymous namespaces
  where possible, RAII ownership (`std::unique_ptr`, no raw `new`).
- Use the C++23 vocabulary where it fits: `contains()` instead of
  `find(...) != npos`, `std::expected` instead of a bool plus an error
  out-parameter, `static` call operators on stateless lambdas passed to
  algorithms, and `std::to_underlying` for enum-to-integer conversions.
- Explicit exception types: `ParseError` (`src/parser.hpp`), `BackendError`,
  `RuntimeErrorLoomwright`, `LexError` (`src/errors.hpp`), `CliUsageError`
  (`src/cli.hpp`), `PickerError` (`src/mousepos.hpp`).
- Keep the DSL small. Do not add arbitrary code execution, shell strings, or
  file/network APIs without a deliberate design change.
- Build warning-clean on
  `-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual`;
  keep it that way.
- Clang is the reference compiler; GCC is supported as well. Keep the code
  warning-clean on both, and use `cmake --preset release-clang` and
  `cmake --preset release-gcc` when a change might behave differently on one
  of them.
- Use `rg` for searching. Keep new code covered by a focused test in `tests/`.

## Docs sync

When user-visible behaviour changes, update the docs in the same commit:

| Change | Update |
| --- | --- |
| Language statement/builtin/option | `src/docs.cpp` (`kGuide` and `api_reference()`), `README.md` if user-visible |
| User-facing behaviour | `README.md` and `src/docs.cpp` |
| Architecture, build, testing, traps | the relevant `docs/*.md` |
| Version (when a bump is warranted) | `CMakeLists.txt` and `packaging/arch/PKGBUILD` |

The repository must never be in a state where the docs disagree with the build.

## Versioning

- Bump the app version in `CMakeLists.txt`
  (`project(loomwright VERSION X.Y.Z ...)`) — this is the source of truth and
  defines `LOOMWRIGHT_VERSION`.
- Keep `packaging/arch/PKGBUILD` in sync (`pkgver=X.Y.Z`). Update the example
  package filename wherever it is mentioned if you change it.
- Choose the bump based on the scope of the change:
  - **Patch** (`X.Y.Z+1`): bug fixes, small internal changes, docs/test-only
    changes that ship with a release, or narrowly scoped behaviour fixes with
    no new user-facing surface.
  - **Minor** (`X.Y+1.0`): backward-compatible features, new language
    statements/built-ins/options, medium/large refactors, new CLI flags, or meaningful user-visible
    improvements.
  - **Major** (`X+1.0.0`): breaking changes to script syntax/semantics, CLI
    behaviour, runtime behaviour that existing scripts rely on, or other
    incompatible changes.
- Do not stack a bump on top of an unpushed bump of the same or smaller scope.
  Before bumping, check `git log origin/main..HEAD` and compare the version in
  the tree with the latest release tag: the current version already reflects
  the highest-scope bump among the commits not yet covered by a release. If an
  earlier unpushed commit bumped the version, keep it unless this change's
  scope is strictly higher (patch < minor < major); raise the version to the
  next tier only for that higher scope:
  - Patch change after an unpushed patch bump: no bump.
  - Minor or major change after an unpushed patch bump: bump to the next minor
    or major version.
  - Patch or minor change after an unpushed minor bump: no bump.
  - Major change after an unpushed minor bump: bump to the next major version.
  - Any change after an unpushed major bump: no bump until that batch is
    released.
- When in doubt, prefer the smaller bump that accurately reflects compatibility
  impact, apply the unpushed-commit rule above, and mention a version bump in
  the change summary only when you actually make one.

## Committing

- Commit your changes once you have written and verified them. Do not leave a
  finished change sitting uncommitted in the working tree for the next agent to
  trip over.
- Verify before you commit: run `make` (build plus the full test suite) and,
  when the change touches the language, `loomwright --check` on every script in
  `examples/`. Commit only what passes.
- Keep the commit focused: one logical change per commit, with a message that
  says what changed and why. Reference the version bump in the message when
  this commit actually bumps the version; a lower-scope commit in the same
  unreleased batch carries no bump.
- Update `README.md`, `src/docs.cpp` (`kGuide` and `api_reference()`), and, when
  a bump is warranted, the version files in the same commit as the code they
  describe.

## Before you start

Read [architecture.md](architecture.md) for the module map,
[implementation-traps.md](implementation-traps.md) for the traps in the area
you are about to touch, and [testing.md](testing.md) for how to verify the
change. The language extension points are in
[language-internals.md](language-internals.md).
