# AGENTS.md

Loomwright is a Linux-first desktop automation language. A `.loom` file is parsed, statically checked, and run by a native C++23 runtime built on one epoll-based event loop (`src/scheduler.cpp`). Input comes from physical keyboards through evdev ioctls on `/dev/input/event*`; output goes through `ydotool`/`ydotoold` or a direct `uinput` device written with plain `input_event` structures. The language never evaluates code or shell strings. `launch` is opt-in and uses an argv vector with no shell.

## Documentation

User docs: `README.md`. Language reference: `loomwright --docs` (`src/docs.cpp`). Examples: `examples/*.loom`.

# Documentation map
Internal docs live in `docs/`; start at [`docs/README.md`](docs/README.md) and read the relevant sections **before** proceeding with the user's tasks (otherwise you'd probably miss some important context).

| Document | What it covers |
| --- | --- |
| [`docs/architecture.md`](docs/architecture.md) | Repo layout, compilation units, lexer→parser→checker→runtime pipeline, scheduler/task model, input/output/GUI subsystems |
| [`docs/build-and-install.md`](docs/build-and-install.md) | Prerequisites, build presets, Makefile targets, CMake options, install layout, Arch packaging, runtime permissions |
| [`docs/gather-context.md`](docs/gather-context.md) | Using `scripts/gather_context.py` to search AGENTS.md, the docs/, and the codebase |
| [`docs/testing.md`](docs/testing.md) | Test-suite layout, running the suite, unit and integration tests, sanitizers, verification commands |
| [`docs/releases.md`](docs/releases.md) | Release packaging, GitHub Actions publishing, and Podman CI testing |
| [`docs/benchmark.md`](docs/benchmark.md) | Measured build/test performance, the optimizations behind it, how to reproduce the numbers |
| [`docs/implementation-traps.md`](docs/implementation-traps.md) | Backend, input and runtime traps: ydotool absolute motion, uinput classification, coroutine stack growth, cancellation, holds, evdev recovery |
| [`docs/language-internals.md`](docs/language-internals.md) | Grammar, AST, statement dispatch, builtins, key names, string decoding, and the extension points for statements/builtins/backends |
| [`docs/cli.md`](docs/cli.md) | CLI surface, exit codes, `--check`/`--run` contracts, mouse-position picker architecture |
| [`docs/contributing.md`](docs/contributing.md) | Code style, docs-sync rules, versioning policy, commit policy |
| [`docs/state-machines.md`](docs/state-machines.md) | Working audit of the implicit state machines and their failure paths |


## Repository rules

- C++23, 4-space indentation, RAII, explicit exception types, and a warning-clean build. See [`docs/contributing.md`](docs/contributing.md).
- Keep the DSL small: no arbitrary code execution, shell strings, or file/network APIs without a deliberate design change.
- Update `README.md` and `src/docs.cpp` (`kGuide` and `api_reference()`) with user-visible changes, and keep the relevant `docs/*.md` in sync. See [`docs/contributing.md`](docs/contributing.md).
- Versioning: bump `project(loomwright VERSION ...)` in `CMakeLists.txt` (the source of truth) and keep `packaging/arch/PKGBUILD` in sync. See [`docs/contributing.md#versioning`](docs/contributing.md#versioning).
- Commit once verified: run `make` (build plus the full test suite) and, when the language changes, `loomwright --check` on every script in `examples/`. See [`docs/contributing.md#committing`](docs/contributing.md#committing). **Read first if the task also involves committing changes at the end.**
- Do not assume the session type, compositor, distro or package manager anywhere — detect at runtime and keep docs generic. See [`docs/build-and-install.md#environment-assumptions`](docs/build-and-install.md#environment-assumptions).
- Use `rg` (if available) instead of `grep` for faster searching.
- If the user implicitly states that you should search for something, and the web search tool is available in your harness, use it to search for information about the task you are working on. For queries, use DuckDuckGo (html) or similar search engines which you can access.
    - If not available, Try `curl` or `wget` to fetch the information.


## Publishing a release

The phrase `publish release` (or "ship what we just did") is enough to run this
whole flow without further prompting. It is only appropriate when **there are
commits not yet covered by a release** (unpushed commits on `main`, or pushed
commits newer than the latest release tag) **and the source version has been
bumped** past the latest published release. If either condition fails, stop and
report why — do not invent a release. Follow
[`docs/releases.md`](docs/releases.md#dispatching-a-release) and
[`docs/contributing.md#versioning`](docs/contributing.md#versioning) for policy.

1. Verify and commit pending changes first (`make`, plus `loomwright --check`
   on every `examples/*.loom` when the language changed). Check
   `git status --short --branch` and `git log origin/main..HEAD`; push to
   `origin/main` once verification and CI are green. A docs-only change with no
   version bump is not a release on its own.
2. Compare `project(loomwright VERSION ...)` in `CMakeLists.txt` and `pkgver`
   in `packaging/arch/PKGBUILD` with `gh release list` and
   `gh release view <tag>`. They must agree on a version newer than the latest
   published release. If the version has not moved, stop and ask for a bump (or
   make one per the versioning policy when the change warrants it).
3. Check the style of the previous release body with
   `gh release view <tag> --json body --jq .body`, then write notes to
   `/tmp/opencode/loomwright-<version>-release-notes.md`. Use the established
   shape: `## Highlights` with short, factual bullets (include measured numbers
   with their source) and `## Before running` for behaviour changes, permissions
   and platform/desktop requirements. The workflow appends `## Downloads` and
   `Built from commit ...`, so leave those out.
4. Dispatch with
   `gh workflow run release.yml --ref main --field description=@/tmp/opencode/loomwright-<version>-release-notes.md`.
   Use manual dispatch, not `gh release create`: the workflow builds, tests,
   packages and checksums every artifact. Watch with
   `gh run watch <id> --exit-status`. If a job fails, fix the cause and rerun;
   do not claim success early.
5. Verify with `gh release view v<version> --json name,tagName,assets,body,url`
   and `gh release list`. The release must be Latest, built from the intended
   commit, and carry the seven assets (Arch package, source, AppImage, deb, rpm,
   tarball and `SHA256SUMS`). Native Wayland integration may skip on runners
   without Weston; that is not a release failure.


## Gotchas

- NEVER use subagents UNLESS the user asks you to use them (whether explicitly at the start of the task or mid-task, user message containing `@explore` or `@general` count as explicit ask).
