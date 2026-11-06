# EDVR — private-fork working rules

This is the repository's agent instruction file. `CLAUDE.md` imports it;
do not duplicate instructions there.

EDVR provides Elite Dangerous: Odyssey graphics fixes through a `d3d11.dll`
proxy and its own OpenXR runtime through `Openvr/win64/openvr_api.dll`.
Test flights are expensive: distinguish local checks from actual game results.

## Repository identity and Entire

Git operations for this checkout belong to
`ProbabilityEngineer/edvr-unofficial-patch`, not the separate DXMT repository.
After a session or checkout move, verify the repository with
`git rev-parse --show-toplevel` and `git remote -v` before making changes.

Entire is enabled here. Consult `entire agent-help`, then
`entire agent-help <command>` for current subcommand usage; do not guess flags.
Approved commits capture the AI session as a checkpoint automatically. Create
manual checkpoints only when requested. Ask before Entire setup or destructive
operations such as enable, disable, clean or auth. Keep automatic session
pushing disabled unless the owner explicitly authorizes changing it.

## Private derivative and publication

This checkout is the owner's private derivative. AI-assisted implementation
is permitted for that work, not as authorization for upstream contributions.
Never push refs to `characterecho-sean/edvr-unofficial-patch`, open upstream
PRs or issues, post comments there, or otherwise publish this work upstream.
This applies to every branch and worktree, including direct URLs, remote
aliases and APIs. Permission to push to the owner's fork is not permission
to publish upstream. Preserve this policy when creating or updating branches.

## Scope and default permissions

Treat the requested outcome as the scope boundary. When implementation is
authorized, relevant inspection, necessary edits, focused regression tests,
local checks and incremental builds using existing toolchains are included.
Do not ask again for each ordinary implementation or verification step.

Do not add unrelated refactoring, cleanup, features or broad test-suite
expansion. Report adjacent problems instead of silently fixing them.
Do not remove or rename a feature or config key without quoting its exact
name and current behavior and obtaining approval.

Ask before:
- Paid compute, remote CI or other externally metered operations.
- Downloading/installing dependencies or changing system/toolchain settings.
- Installing artifacts or changing live game settings.
- Committing, merging branches, pushing, publication or destructive operations.
- Substantial unrelated work or unusually expensive local validation.

Before an operation that triggers remote CI or publication, disclose its
expected downstream actions and obtain approval covering them. Approval of
the initiating operation does not authorize undisclosed downstream work.
There is no automatic commit, merge, push, issue-creation or release step.

## Proportional verification

Use the smallest set of checks that exercises the requested behavior and
guards likely regressions. Prefer existing test infrastructure. A focused
regression test and a relevant compile/build check are normally sufficient
for development, not proof of runtime correctness or release readiness.

Demonstrate failure before the fix when practical. Where the behavior needs
the real runtime, explain that limit instead of manufacturing a substitute
claim. Diagnostic changes and candidate fixes may precede runtime confirmation.
State the hypothesis and the observation that would confirm or refute it.

If a check exposes a defect in the requested change, fix it and rerun that
check. Report unrelated failures without expanding scope. Stop when the
requested work and relevant checks are complete, or report the specific
blocker and remaining uncertainty.

Do not recursively test test helpers, validate validators or add planning
layers merely for confidence. Run a tool's relevant self-tests when changing
the tool; ordinary use does not require revalidating it. New repository
tools should use the existing self-test convention rather than a new framework.

Use available local checks on the current host. Do not automatically dispatch
Windows CI for each C++ edit. Full Windows validation is required to claim
Windows validation or release readiness, not before every local edit or commit.
Never bypass, weaken or forge build gates, receipt checks or safety checks.
If a required project gate exceeds the approved scope, explain once and ask.

## Build and tooling safeguards

- Use the host's supported shell. PowerShell 5.1 restrictions apply only there.
  When using it, avoid native stderr redirection and pipelines that terminate
  file-writing tools early; preserve UTF-8 and native argument quoting.
- Invoke `build.bat` by absolute path and verify that it actually ran.
  Use its `--wait-lock` option; do not wait separately and race another build.
- The full build runs tests and installer gates and produces a receipt.
  `--dll-only` is receipt-guarded; it is not a shortcut around validation.
  Compile-only outputs are not validated release artifacts.
- For an explicitly unvalidated graphics test DLL, the supported path is
  `--compile-only --graphics-only`, or manual CI input `graphics_only=true`;
  see `docs/building.md`. This does not require full validation and is not a
  bypass of receipt-guarded promotion. Remote CI still requires approval.
- Use `tools/worktree.py` for build-capable worktrees. Never use a bare
  stash/pop across worktrees; the stash stack is shared.
- Read targeted source and documentation ranges. Delegate only when useful
  and supported, not as a mandatory model-escalation or review process.
- Read-only source, log, config and Git inspection may use ordinary tools.
  Prefer `tools/edvr_log.py` for structured flight reports.
- Existing tools own installation, target resolution, artifact verification
  and build receipts. Reuse them instead of ad-hoc replacement scripts.
- `--dry-run` must write nothing, including temporary files or directories.

## Runtime diagnosis and deployment safety

- Establish which build produced a flight log before attributing behavior.
  Distinguish "never ran", "refused", "accepted" and completed/correct output.
- Preserve hook displacement/relocation checks, protection transitions,
  publication ordering, cleanup and bounded recovery behavior.
- Trace new instrumentation through its actual execution/reporting path.
  Make absence of execution distinguishable from success.
- Prefer root causes over cosmetic compensation. State the relevant runtime,
  rendering dimensions and feature state; do not demand extra test flights
  when existing evidence answers the question.
- Record important ruled-out hypotheses briefly in an existing investigation
  document when needed to avoid repeated work. Do not create an investigation
  archive for every small change.
- Deployment needs separate approval. Use `tools/install_edvr.py` and its
  verification mode; do not copy over a running game or infer identity from
  timestamps. Preserve artifact and receipt safeguards.
- Live `edvr.ini` and `edvr-flat.ini` are user state. Do not overwrite them
  during unrelated installation or edit them with broad regex replacements.
  For authorized changes, inspect the exact keys and review the resulting diff.
- Config keys describe user-facing functionality, not implementation details.
  Avoid permanent switches solely for a temporary diagnostic experiment.

## Private analysis and shared state

`analysis/`, when present in the main checkout, is a separate private
repository. Read its own `AGENTS.md` before working there. Game executables,
Ghidra projects and decompiled material must not be published in this fork,
its issues or release artifacts.

Do not symlink/junction analysis into a worktree or bypass analysis write
guards through a shell, another clone or another path. Worktree sessions may
read it but must use its documented intake process for proposed writes.
An EDVR commit approval does not authorize commits in the analysis repository.
Protect shared outputs from silent replacement by another session or process.

## Reporting and documentation

Briefly report what changed, the checks run and their results, and material
limitations. Distinguish verified behavior from hypotheses and identify the
tested revision or working-tree state. Do not enumerate every unrelated check.

Do not create extra plans, reports, manifests or evidence archives unless
requested or necessary. This does not prohibit runtime artifacts required by
the requested behavior, generated assets, or normal build/test output.
Do not repair unrelated documentation structure as part of an implementation.
Agent-only handoffs belong under `.agents/`, not source comments or release
notes; keep agent material `export-ignore`. Human-facing docs belong in `docs/`.

## References and layout

- `CODING_STANDARDS.md`: relevant code-review and correctness rules.
- `docs/building.md`: detailed platform, build, receipt and packaging recipes.
- `docs/windows-release-ci-2026-10-18.md`: Windows CI background, not permission
  to dispatch it. Release packaging and credits apply only to authorized releases.
- `src/d3d11/`: graphics hooks and rendering fixes; `src/common/`: shared code.
- `src/openxr/`: OpenXR runtime; `src/openvr/compat/`: OpenVR ABI.
- `src/installer/`: installer; `tools/`: existing tooling and test infrastructure.
- `build.bat` / `package.bat`: official full-validation and packaging entry points.
