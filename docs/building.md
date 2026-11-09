# Building EDVR

## Graphics-only diagnostic compile

For a private Windows diagnostic DLL, invoke `build.bat` by absolute path with
`--compile-only --graphics-only --wait-lock --jobs 4`. This uses the normal build
lock and graphics compiler/link recipe, but exits before the graphics alias,
OpenXR/OpenVR runtime, installer and packaging steps. It writes only the EDVR
graphics DLL to `build/graphics-compile/d3d11.dll`; shader/compiler helpers and
intermediate files stay in that separate directory. It does not copy vendor
runtime DLLs or the OpenXR loader into that output directory.

This is **not full Windows validation or a release artifact**. It emits no
full-build receipt, does not run the test rigs, and is not a substitute for the
receipt-guarded `--dll-only` promotion route. Existing full-build and Linux
compile-only behavior is unchanged.

The owner's fork can run this through the existing Native Windows build
workflow's manual `graphics_only` input. That selects a separate job, skips
the normal validation/package job, verifies pinned graphics dependencies,
and uploads `d3d11.dll` as a diagnostic artifact. It also uploads build logs.
Running CI and installing the resulting DLL still require separate approval;
the workflow neither installs anything nor creates a release.

### Private crosshair capture diagnostic

The crosshair diagnostic uses the existing **full draw-capture trigger**
(Shift + the configured `dump_draws` key). Aim the game's **centred crosshair**
at a surviving, less-transparent decoration and hold the view steady. No new
overlay or config key is added. Eight contiguous 16×16 windows cover a centred
64×32 render-pixel region. First, one qualified live frame counts eligible draws
without pixel, constant-buffer, pool or reference copies. The next two qualified
frames sample each class at a stride of `ceil(calibrated_count / 256)`, with
separated phases. For this diagnostic, `scene` means depth-writing and `motion`
means non-depth-writing, so opaque engine-motion draws no longer consume the
non-depth-writing quota. Non-depth-writing draws remain eligible regardless of shader
family, but no longer bypass the stride and exhaust the quota early.

Each class still has 256 draws/frame, with two sampled frames and the existing
256 MiB/30-second/900-frame bounds. Later captures in the same process advance
the sampling phases; they do not claim stable draw identity or guaranteed full
coverage. Metadata records calibration counts, actual counts, strides, phases,
and both sampling and quota omissions. Population growth can still exhaust a
quota and is reported. Source/extent changes require fresh calibration (or
cancel an already-started two-frame sample).

Each accepted frame also attempts a same-frame, pre-Present central output
crop, at most 512×512 pixels. This currently supports RGBA8/BGRA8 SDR output;
unsupported formats and missing/failed references are explicit, not success.
To export RGB reference PNGs without altering capture files:
`python tools/flat_draw_pixels.py CAPTURE_DIR --preview-dir NEW_OUTPUT_DIR`.
The output directory must be new and outside the capture; `--dry-run` writes
nothing. Metadata records render/output extents and the sampled region's
position in the crop. Old eight-point captures remain readable.

This is contributor discovery, not a rendering fix or automatic selection of
a replay-ready packet. A changed sample alone does not establish decoration
ownership; use the matching reference and draw chronology.

How to build EDVR from source, check the build without the game or a headset,
and what the optional DLSS mode and the release package need.

Building needs Visual Studio 2022 with the C++ workload, and Python with the
numpy and Pillow packages, which several of the tools the build runs import:

```
pip install -r tools\requirements.txt
```

For a release-capable build, fetch all four pinned dependencies. The fetch
tools verify their sources and staged files; the build verifies them again:

```
python tools\fetch_ngx.py
python tools\fetch_ffx_dx11.py
python tools\fetch_xess.py
python tools\fetch_openxr_loader.py
```

Then build:

```
build.bat --wait-lock --jobs 4
```

That produces the native graphics/runtime pair, `build\openxr_loader.dll`, and
`build\smoke.exe`; the last of these checks the build without the game or a
headset:

```
build\smoke.exe build\d3d11.dll
```

A full build leaves your desktop alone, so you can keep typing in another
window while it runs. The runner holds every test exe to that and fails the
build if one shows a window or takes the keyboard focus; see
`docs\build-focus-steal-2026-09-29.md`.

For a native Windows build from Linux, use the **Native Windows build**
workflow (`.github/workflows/release.yml`). It also runs by itself on every pull
request and every push to `main` (a `codex/release-*` pull request is left to
its branch's push run). It runs on Windows Server 2022 with
Visual Studio 2022, Python 3.12, and all four pinned dependencies. It invokes
the full `build.bat --wait-lock --jobs 4`, including every test rig and the
installer gate, then calls `package.bat`. The packages contain the exact
version stamp checked by `package_native.py`. Each successful run uploads the
full build receipt, installer, native DLLs, three release ZIPs, and
`SHA256SUMS`. Build logs, phase timings, and failed flat pixel fixtures are
uploaded separately even when a gate fails. The workflow does not create a
GitHub release or use signing secrets.

A push to a `codex/release-*` branch runs the entire build and package path. If
the commit already has a release tag, the runner uses its exact version;
otherwise it creates a temporary local tag, `v0.0.0-ci`, before building. This
makes the version stamped into the DLLs and installer match the package
version. The temporary tag exists only on the runner; either branch result is a
proof artifact, not a release asset. The branch push trigger works before this
workflow reaches `main`. GitHub does not offer its manual **Run workflow**
control for a new workflow until the workflow file is on the default branch.

### Linux: compile-only builds under Wine

For a test flight on a Steam Deck or another Linux machine that runs Elite
under Proton, `tools/linux_toolchain.py` compiles the production DLLs with
Microsoft's compiler under Wine. It runs `build.bat --compile-only`, which
skips every test, rig and installer gate, so the result is not validated and
not a release asset: a merge or a release still needs CI's full build. Use host
`python3`, from any shell:

```
python3 tools/linux_toolchain.py setup --dry-run
python3 tools/linux_toolchain.py setup --accept-license
python3 tools/linux_toolchain.py doctor
python3 tools/linux_toolchain.py build-dlls [--root <worktree>]
```

`setup` needs the system packages `wine msitools git`, which you install
yourself (it never runs sudo), and downloads VS 2022's Build Tools, a Windows
Python and the pinned SDKs. The Build Tools are under Microsoft's licence:
using them needs a valid Visual Studio licence, and they must never be shared,
published or committed. `--accept-license` says you have read and accept those
terms; without it `setup` stops before downloading. Everything lives in the
Git-ignored `.toolchains/windows/` of the main checkout (`EDVR_TOOLCHAIN_HOME`
moves it) and serves every worktree. A tree's output, a worktree's included, is
`build/compile-build` in that tree, with a `compile_only_receipt.json` that
`install_edvr.py --compile-only` checks (`tools/compile_only.py`).

It differs from CI's build in a few ways: the MSVC (14.44.35207) and Windows
SDK (10.0.26100) are CI's own; shaders compile with the SDK's own
`d3dcompiler_47.dll`, not Wine's; the proxy's export list comes from Wine's
`d3d11.dll`, which lacks some exports Windows has (Elite imports
`D3D11CreateDevice` alone from it); the FSR 3 DX11 port builds with CMake and
Ninja (`fetch_ffx_dx11.py --generator ninja`), not the Visual Studio generator;
and no gates run. The docstring of `tools/linux_toolchain.py` has the details.

### The caches in CI

The workflow keeps two caches (`actions/cache`), and a third file set inside
the second. Measured 2026-10-22 on the same branch: a cold run took 16.5 min
(build step 12.9 min), the next run on a new commit 8.8 min (build step 6.7
min); the unchanged workflow on `main`, run alongside, took 22.3 min (build
step 17.9 min). Runner speed varies a good deal between runs (the same cold
build was 20 min a day earlier), so compare runs made together.

- **Pinned dependencies** (`third_party\ngx`, `ffx-dx11`, `xess`,
  `openxr\loader`; 229 MB). Key: the runner image and the hash of the four
  `tools\fetch_*.py` files, which hold the pins. A hit skips the fetch and the
  FidelityFX build (2.7 min); the `--verify` step runs on a restored copy
  exactly as on a fetched one, and a failed fetch or verify never saves.
- **Compile cache** (about 100 MB compressed). Key: the runner image, the
  branch and the commit; a run restores the newest cache of its own branch,
  else of the default branch (GitHub shows a run only its own ref's caches and
  the default branch's, so one `gh workflow run release.yml --ref main` after a
  merge seeds every branch). It is saved after the build even when a rig
  failed, so a rerun after a flake starts warm. Release tags (`v*`) neither
  restore nor save it: they compile from scratch. The build log's
  `compile_cache:` lines are in the `edvr-diagnostics` artifact; the run's job
  summary carries the totals (`python tools\compile_cache.py --summary LOG`).
- **Riding with it**: `build\rig_times.json`, so the longest rigs start first
  and timeouts are set from the last run, and the two generated shader headers,
  which carry their own key over the HLSL and the compiler DLL and regenerate
  when it differs (about 60 s saved).

Why a hit is safe. `compile_cache.py` already decides every compile on a hash
of the source, of every project header the compiler reported, of the flags, of
the compiler's identity and of INCLUDE, LIB and CL. Nothing about that changes:
a restored entry for an input that changed is a miss, and the tool's self-test
proves it. What the archive adds is a file whose size and time the tool cannot
trust, so `--pack` stores only entries whose outputs still match their
manifest, and `--unpack` installs an entry only when each output's sha256
equals the one recorded when cl wrote it, then sets the file's time back; an
entry that fails is left out and recompiles. The cache skips compiles only:
every rig still runs on every build. Proof builds carry one version stamp,
`0.0.0-ci`, because the stamp is in a header that `log.cpp`, `menu.cpp` and
several other headers and sources include, and a stamp that changed per run
missed all of them. Two things still recompile every run: the installer (its
payload embeds the freshly linked DLLs) and `object_classification_test` (an
`ml64` object carries a timestamp).

After the workflow is on `main`, a manual branch run is also possible:

```powershell
gh workflow run release.yml --ref main
```

These examples use PowerShell syntax compatible with Windows PowerShell 5.1. On
Linux, run them in `pwsh` with GitHub CLI installed. Inspect a proof run and
download its packages. Use the run ID shown by `gh run list`; a rerun
increments `run_attempt`:

```powershell
gh run list --workflow release.yml --branch codex/release-workflow --limit 5
$runId = 'RUN_ID'
gh run view $runId --log-failed
$repoName = gh repo view --json nameWithOwner --jq .nameWithOwner
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve repository name' }
$runAttempt = gh api "repos/$repoName/actions/runs/$runId" --jq .run_attempt
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve run attempt' }
New-Item -ItemType Directory -Force -Path "edvr-artifacts/$runId" | Out-Null
gh run download $runId -n "edvr-packages-$runId-$runAttempt" -D "edvr-artifacts/$runId"
if ($LASTEXITCODE -ne 0) { throw 'Package download failed' }
Push-Location "edvr-artifacts/$runId"
try {
    sha256sum -c SHA256SUMS
    if ($LASTEXITCODE -ne 0) { throw 'Package checksum verification failed' }
} finally { Pop-Location }
```

If a run fails, download `edvr-diagnostics-$runId-$runAttempt` in the same way
and read its build logs and saved fixture before changing a gate. The complete
Actions job log is also available through `gh run view $runId --log`.

For a real release, merge and push the validated source to `main`, then make
and push its exact version tag from a clean checkout. A pushed `v*` tag runs
the same full validation and packaging at that tag; the workflow refuses a tag
whose name is not a semantic version:

```powershell
git switch main
if ($LASTEXITCODE -ne 0) { throw 'Cannot switch to main' }
git pull --ff-only
if ($LASTEXITCODE -ne 0) { throw 'Cannot update main' }
if (git status --porcelain) { throw 'Checkout must be clean before tagging' }
$version = 'VERSION'            # for example '0.20.0'
git tag "v$version"
if ($LASTEXITCODE -ne 0) { throw 'Cannot create version tag' }
git push origin "v$version"
if ($LASTEXITCODE -ne 0) { throw 'Cannot push version tag' }
gh run list --workflow release.yml --limit 10
$runId = 'TAG_RUN_ID'
gh run watch $runId --exit-status
if ($LASTEXITCODE -ne 0) { throw 'Tagged Windows build failed' }
$repoName = gh repo view --json nameWithOwner --jq .nameWithOwner
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve repository name' }
$runAttempt = gh api "repos/$repoName/actions/runs/$runId" --jq .run_attempt
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve run attempt' }
New-Item -ItemType Directory -Force -Path "edvr-artifacts/$runId" | Out-Null
gh run download $runId -n "edvr-packages-$runId-$runAttempt" -D "edvr-artifacts/$runId"
if ($LASTEXITCODE -ne 0) { throw 'Package download failed' }
Push-Location "edvr-artifacts/$runId"
try {
    sha256sum -c SHA256SUMS
    if ($LASTEXITCODE -ne 0) { throw 'Package checksum verification failed' }
} finally { Pop-Location }
```

Write and review release notes before creating the draft. Generate the Thanks
section from the previous tag, then write each contributor's sentence by hand.
Only merged PR authors are named, plus anyone the maintainer approved by name
in `tools/release_credits_approved.json` for a commit in the release. Issue and
log reporters get the tool's general line. Review the final notes file, then
attach the three verified ZIPs and `SHA256SUMS` to a draft release:

```powershell
python tools/release_credits.py --from vPREVIOUS --to "v$version" --thanks --out thanks.md
if ($LASTEXITCODE -ne 0) { throw 'Review release credits before continuing' }
# Edit release-notes.md and include the reviewed ## Thanks section from thanks.md.
$assets = @(
    "edvr-artifacts/$runId/edvr-$version.zip"
    "edvr-artifacts/$runId/edvr-installer-$version.zip"
    "edvr-artifacts/$runId/edvr-installer-linux-$version.zip"
    "edvr-artifacts/$runId/SHA256SUMS"
)
gh release create "v$version" @assets --draft --verify-tag --notes-file release-notes.md
if ($LASTEXITCODE -ne 0) { throw 'Draft release creation failed' }
```

For a prerelease tag, add `--prerelease` to `gh release create` as well.

The draft is the review point before publication. A failed build has no package
artifact and should never be used for a release.

The optional DLSS mode, and an installer that carries NVIDIA's runtime, need
the DLSS SDK on the machine. Its licence keeps it out of this repository, and
it is not in the graphics driver either. This command

```
python tools\fetch_ngx.py
```

fetches one pinned commit of NVIDIA's public SDK repository into
`%LOCALAPPDATA%\EDVR\ngx-sdk` and checks the runtime's hash against the pin in
the script. `EDVR_NGX_SDK` points the build at a copy somewhere else.

Generating the installer's resources needs the native pair and the bundled
loader; there is no legacy OpenVR-only package. `package.bat <version>
--no-dlss` packages a build made without the DLSS SDK. When the build contains
DLSS, the archive keeps its matching DLL and NVIDIA license notice next to the
installer that embeds it.
