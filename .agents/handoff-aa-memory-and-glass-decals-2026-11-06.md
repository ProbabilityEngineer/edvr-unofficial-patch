# Handoff to Claude: AA-associated memory growth and missing glass decals

> **Archived investigation, 2026-11-08:** the owner has directed us to assume
> capacity pressure on the 36 GB machine rather than a leak. The newer RC7
> flight showed an allocation plateau, a supersampling-related step and
> subsequent recovery. Do not resume the memory-isolation plan below by
> default. It is historical context, not an active task. No bounce-bypass
> flight result was supplied. HDR and AA work remain separate active topics.

## Start here

Continue in `/Users/sam/git/forks/edvr-unofficial-patch`.
Read `AGENTS.md` and relevant `CODING_STANDARDS.md` rules first.

- **Current branch:** `private/rc6-aa-bounce-isolation`
- **HEAD:** `f97195c3bb1a44d5feafc3de1134d5e3d064e06a`
- **Last flown diagnostic:** `6b6b3978`, engine-record isolation only.
- **Latest supplied DLL:** `f97195c3`, engine-record isolation plus map-bounce bypass.
**No flight result for the bounce-bypass DLL has been supplied yet.**

The working tree was clean before this handoff was written. The source changes,
tests and investigation notes through preparation of the bypass are committed.
This handoff is a new, uncommitted file. `.agents/**` already has `export-ignore`.

Human-facing investigation:
`docs/edvr-unified-hdr-2026-11-01.md`, particularly:

- “HDR-output-off / 1.5x supersampling flight”
- “AA-off control”
- “AA-enabled pre-resolve path review”
- “Prepared AA allocation-scope control and record-velocity isolation”
- “Record-isolation flight”
- “Memory investigation: current conclusions and next step”
- “Focused buffer-interception source review after the isolation flight”
- “Prepared bounce-only bypass candidate”

## Problems and the important distinction

1. **Reported progressive memory growth / memory pressure with EDVR AA selected.**
   The user says it does not happen without EDVR and could not reproduce it with
   `fix.temporal_aa = off` in the INI.
2. **Decals/signage disappear on station glass windows when AA is selected.**
   They returned in the engine-record-isolation flight, while the memory
   problem persisted. Treat these as separate investigations now.
3. **HDR too dark at default gamma** remains an independent unresolved issue.
   Do not retune gamma/HDR/display settings as part of the memory work.

“Leak” is the user's description of the symptom, not yet a demonstrated missing
release. We have measured rising live texture objects, but the no-symptom AA-off
control also loads substantial texture data. Do not infer leaked references
solely from increasing live bytes or blame the game merely because it created
the textures. EDVR can retain game-created resources or change game/driver
behavior. Conversely, bounded EDVR allocations do not prove all interception
paths harmless.

## What has been tried

### Earlier fixes and exclusions

- The current line descends from RC6 with separate engine-hook allocation and
  HDR-output fixes integrated. Keep those independent fixes intact.
- Definite HDR fixes corrected HDR-local shader identity hashing, added the
  captured no-bloom tone variant, and corrected exposure-bias subtraction.
  They did not eliminate the memory symptom. Default gamma was preserved.
- A separate Present correction forwards `DXGI_PRESENT_TEST` without frame
  work. Reproducing flights have **TEST count zero**, so this fix was not
  exercised by the reproduction.
- Growth occurred before ordinary occlusion in a comparison flight.
  AA-off also accumulated thousands of occluded Presents without the reported
  problem. Do not restart the “nothing retires while occluded” theory or add
  repeated temporal resets on positive occlusion.
- The user restored bundled/original CrossOver DXMT and still reproduced.
  Custom/nightly DXMT and the custom MetalFX4 path are not necessary.
- Earlier DXMT retirement diagnostics showed submitted work retiring; fixed
  native probes did not reproduce the game/Wine workload. These observations
  do not clear every driver backing-storage interaction.
- A DXMT memory-budget hypothesis was considered but not demonstrated.
  The no-EDVR/no-AA controls remain central; do not replace them with a generic
  “the game is just caching textures” explanation.

### Texture-lifetime diagnostic, `8b001cc0`

Non-retaining private-data `IUnknown` tags on successful hooked Texture2D
creations count creation/final tag destruction. A tag does not hold its texture.
The module is pinned and ledgers intentionally survive process teardown.
Reporting is bounded; attachment failures/cap skips are visible.

Creation origin is the caller image, **not the retaining owner**. Byte counts
are estimates, not physical Metal allocation. Coverage excludes pre-hook
resources, swapchain images and other creation entrypoints. Descriptor overflow
preserves totals but loses descriptor detail.

- `edvr_gfx_20261106_160852.log`: external live textures increased
  5879 -> 6701 and approximately 3282 -> 4966 MiB, while EDVR-created textures
  stayed at 49 / 1473 MiB. Growing groups included mipmapped BC4/BC5/BC7
  sampled-only material textures, outside the HDR-widening predicate.
- Source reviews found bounded FSR/HDR/resolver resources and no demonstrated
  missing raw getter reference. Production `FlatContextState` capture,
  recapture and abandonment tests balanced fresh compressed-texture lifetimes.
  This is not proof that the entire hooked game/DXMT workload is correct.

### HDR off, AA selected, 1.5x supersampling

`edvr_gfx_20261106_165621.log`, same `8b001cc0` build:

- Output 3024x1964, scene became 4536x2946: 1.5x each axis, 2.25x scene pixels.
- External live texture estimates rose 4990 -> 6882 MiB over two minutes;
  EDVR's set stayed at 37 textures / 1131 MiB through most of the later run.
- During 74 supersampled route windows, 6051 frames declined before capture /
  backend treatment. This does **not** mean all AA machinery was disabled.
- Engine shader substitutions and foreground capture still ran heavily.
  The user saw smoothing and missing glass decals.
- **Correction to an earlier assistant overstatement:** the first observation
  refusal was `projection-preparation-refused` at 16:56:43, before the 16:58:04
  supersampling change. Resolution is not established as the cause of refusal.
  Supersampling itself can smooth the image, but the log cannot attribute all
  visible smoothing to it.
- “Flat HDR route” refers to the game's internal scene processing, not proof
  that display HDR remained enabled.

### AA-off control

`edvr_gfx_20261106_173558.log`, same `8b001cc0`:

- INI and runtime confirm `temporal_aa = off`; HDR off.
- User could not reproduce the memory symptom.
- EDVR textures: 3 created, 3 destroyed, zero remaining.
- External texture turnover largely balanced over a later three-minute
  interval: 6765 -> 6787 live, nearly unchanged estimated bytes.
- There were substantial initial loading and later resolution changes.
  This reinforces that external texture growth alone is not leak-specific.
- UI-quality, sharp-weapons and shield-ripple paths reported waiting because
  AA was off. No FSR context creation or engine-motion reports.

## Engine-record isolation: the strongest new result

**Revision:** `6b6b3978`, build `6AEE5B22`

**Flight:** `edvr_gfx_20261106_230550.log`

User: **AA did not visibly work, glass decals returned, memory problem remained.**

The diagnostic verified, across all ten reports:

- `isolate-engine-records=1`
- `engine-active=0`
- Zero pinned game buffers and zero record substitutions.
- Zero resolver attempts; 11762 resolver preflight attempts by the last sample.
- Route refusals included `engine-source-not-ready`, explaining absent AA
  treatment rather than merely assuming it from the screenshot.

The run began in TAA (`mode=on`) at scene/output 3024x1964. Around 23:09:10–14
the user cycled DLSS, FSR, XeSS, off, then on. It is not a fixed-settings growth-
rate comparison with the earlier FSR/1.5x flight.

**Before the mode cycle**, 23:06:50 -> 23:08:50:

- External estimated live texture bytes: 2292 -> 3930 MiB.
- Foreground capture attempts: zero.
- Independent domain-marker attempts: zero.

Thus another foreground-off flight is not needed simply to establish growth
without foreground capture.

Brief later foreground activity made 22 buffers; all 22 were destroyed by
23:09:20. It issued 372 domain markers and then stopped.
At the final samples, measured owner totals were stable:

| Allocation scope | Live textures | Estimated texture bytes | Live buffers / bytes |
| --- | ---: | ---: | ---: |
| Engine/domain helper | 1 | 95026176 | 1 / 16 |
| Resolver | 8 | 172234944 | 1 / 336 |
| Other EDVR | 4 | 55691664 | 4 / 976 |
| Foreground | 0 | 0 | 0 / 0 |

The engine-labelled allocation is from the independent domain helper, not
evidence that record isolation failed. All tracking failure/skip/pending
counts were zero. These are allocation-scope labels, not final ownership proof.

**Conclusions:**

- The disabled engine-record group is implicated in the glass regression.
  VS `BBE58E40FE88EC80` / PS `DB3E8D20CF53FBC0` is a previously flagged decal
  family that ran substituted in earlier flights, but it has **not** been
  established as the specific glass-signage draw.
- The memory symptom does not require record substitutions/private copying,
  completed temporal resolve, or sustained foreground capture.
- Remaining AA-enabled work includes projection tracking and Map/Unmap bounce.
  Late windows still showed about 70000 bounced maps and 166 MB of copy traffic
  per five seconds, with zero open mappings at Present. **Traffic is not a
  measured leak rate.**

## Focused source review: what was and was not found

Relevant sources:

- `src/d3d11/vscreen.cpp`: real Map/Unmap interception; bounce installation,
  shadow observation, lease completion, original-call forwarding.
- `src/d3d11/flat_runtime.cpp`: `FlatMapBounceD3DDriver`, bounce adapters,
  projection lifetime/work tiers and one-time bounce-mode selection.
- `src/d3d11/flat_map_bounce.h`: portable core, eight slots, lease lifecycle.
- `src/d3d11/flat_projection_runtime.{h,cpp}`: registration, eviction, plans,
  full-write capture, reset.
- `src/d3d11/flat_projection_bindings.h`: shadow bank.
- `src/d3d11/flat_projection_scope.{h,cpp}`: private buffers and saved bindings.

Normal paths inspected:

1. Registration accepts aligned constant buffers of 16..65536 bytes and retains
   at most 64 in a fixed table. QI results are balanced by local ComPtrs.
2. The shadow bank allocates its 4 MiB payload once. Bounce payload and baseline
   arrays occupy approximately 1 MiB total, reused rather than growing per Map.
3. Each installed bounce retains a resource/context pair. Its RAII Unmap lease
   flushes to the original driver pointer and releases those references before
   the real Unmap. Internal and foreign-context hook paths were traced too.
4. Remapping drops abandoned slots. Wrong-context Unmap preserves a bounded
   outstanding slot for the correct context and trips; overdue mappings trip
   at Present. The actual flight does not show those exceptional conditions.
5. Tracking evicts eligible unpromoted/unmapped/nonpending entries; `discard`
   releases shadow identity, private-buffer state and original ComPtr.
6. Work-tier departure or resize/stop destroys projection state. The fixed
   bounce core intentionally survives so outstanding mappings can still finish.

**Caveat found, not fixed:** `retirePlan` removes logical pins but does not
immediately clear the saved original/replacement ComPtrs in that plan. They can
survive until plan reuse or runtime destruction. This is bounded by the plan
arrays, not a demonstrated unbounded leak. No evidence ties it to this flight's
growth. Do not conflate resolver-preflight counters with projection-plan
creation counters.

**Local checks:**

- Existing native `flat_map_bounce_test --self-test`: passed.
- Actual projection runtime/scope implementation files: x64 syntax passed.
- Full existing projection-test header could not build locally because it pulls
  in missing generated `temporal_shader_bytecode.h`.
- A focused temporary driver at
  `build/aa-buffer-investigation/projection_test.cpp` instead linked the actual
  projection implementation. It exercised 576 fresh 64 KiB dynamic buffers,
  real discard Map/Unmap, registration, table eviction, reset/reinitialization
  and destruction. All 2312 checks passed: at most 64 unpromoted buffers / 4 MiB
  retained, and all 576 lifetime tags destroyed.
- That probe does not exercise active cached plans or the hooked Elite draw
  pipeline. It ran under CrossOver; requested WARP driver type 5 was reported
  unsupported while device creation succeeded. **Do not call it true WARP
  validation.** Its source/binaries/logs are local ignored build artifacts.

No missing release or unbounded shadow allocation was demonstrated.

**Why bounce is still worth isolating:** it is not passive observation.
It replaces the game's mapped pointer, seeds it from a previous shadow or
zeros, and copies the full buffer back before Unmap. Balanced COM references
cannot prove this behavioral interaction harmless, nor measure physical
driver backing allocations.

## Current experiment: bounce bypass awaiting gameplay

`f97195c3` keeps `kIsolateEngineRecords = true` and adds
`kBypassMapBounce = true` in `src/d3d11/aa_memory_scope.h`.
`flatRuntimePresent` selects existing `flatmap::Mode::Off` instead of Auto.
No permanent INI key was added.

**Intentionally unchanged:** the original Map/Unmap hooks, projection observation,
buffer registration, independent foreground/domain work, and outstanding-lease
cleanup. This is the next variable relative to the flown record-isolation
variant—not a return to working temporal AA.

The extended existing map-bounce rig passes natively and under CrossOver:
Off cannot be re-enabled by adaptive sampling; original pointer/unwritten
bytes survive; game writes reach original Unmap; new mappings have no bounce
retention/writeback; an already-open lease still flushes/releases after Off.
These are core/fake-driver tests, not an Elite reproduction.
ARM64EC diagnostic-test TU syntax also passed.

**Approved CI completed once, no retry:**

- Run: https://github.com/ProbabilityEngineer/edvr-unofficial-patch/actions/runs/42313697660
- Graphics-only Windows job: success, 6m59s.
- Artifact: `edvr-d3d11-compile-only-42313697660-1`
- Local DLL:
  `/Users/sam/git/forks/edvr-unofficial-patch/build/rc6-aa-bounce-isolation-ci-42313697660/d3d11.dll`
- SHA-256:
  `99671e1db64af1ddc8711aceca7b5899978da51d97ec8c9c6d6a1252c7668f61`
- Only `d3d11.dll` was built as a runtime target; the artifact also contains
  license texts. No installer/package/other runtime DLL build.
- Agent did **not** install it. User installation has not been confirmed.
- This is Windows DLL compilation, **not** full test/installer/receipt validation.

For the next supplied log, first establish exact DLL identity, then verify:

1. AA selected, preferably initial TAA `on` matching the last run; do not ask
   for AA-off again. Engine-record isolation is intentionally still active.
2. `bypass-map-bounce=1`, and after flat configuration, actual bounce state
   `off`, key `off`, zero bounced mappings/flushes.
3. Projection tracked mappings/full-write traffic still occurring. Absence of
   all work would make this an overbroad isolation, not a clean bounce result.
4. Comparable HDR, scene/output dimensions and gameplay; avoid mode cycling
   during the comparison. Distinguish texture-object totals from system-wide
   pressure, per-process memory and physical GPU allocation.

**Interpretation:**

- Symptom persists with verified zero bounce activity: bounce is not necessary;
  inspect/isolate remaining projection/resource observation next.
- Symptom disappears while observation continues: bounce's pointer/seeding/
  writeback interaction is implicated, not yet a specific leak or driver bug.
- Neither outcome justifies changing glass-decal code or adding blanket resets.

## Branches and commits

- `private/rc6-hdr-texture-lifetime`:
  - `8b001cc0`: original texture tags, last baseline diagnostic DLL.
  - `1d9f1480`: production context-capture lifetime tests.
  - `ebdf8d4b`: AA-off and source-investigation notes.
- `private/rc6-aa-owner-diagnostics`:
  - `7a23fc74`: measured control, owner-scoped resource capacity and execution.
- `private/rc6-aa-engine-isolation`:
  - `6b6b3978`: record isolation, flown.
  - `86ae14a4`: isolation-flight findings/next steps.
  - `42351200`: focused buffer source review.
- `private/rc6-aa-bounce-isolation`:
  - `f97195c3`: bounce bypass, tests and notes; current, pushed to owner fork.

Not every local branch tip has been separately pushed as a ref. The current
bounce branch contains the ancestor commits. Do not push all branches blindly.

## Logs, tooling, and permission boundaries

Logs directory:

```text
/Volumes/library-video-2021/x-dont-backup/games/Bottles/Elite-Arm/drive_c/Program Files (x86)/Frontier/EDLaunch/Products/elite-dangerous-odyssey-64/edvr_logs/
```

Use `python3 tools/edvr_log.py --file <path> --version` and bounded grep/report
queries. Avoid dumping whole multi-megabyte logs. Relevant flights are named
above; earlier bundled-DXMT comparison is `edvr_gfx_20261106_150150.log`.

Local toolchains:

- Native: `xcrun clang++` (bare `clang++` may select the wrong toolchain).
- `/Users/sam/llvm-mingw/bin/x86_64-w64-mingw32-clang++`
- `/Users/sam/llvm-mingw/bin/arm64ec-w64-mingw32-clang++`
- CrossOver:
  `/Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver/bin/wine`
  with `--bottle Elite-Arm --no-update`.

Owner fork is `ProbabilityEngineer/edvr-unofficial-patch`. **Never publish
upstream** to `characterecho-sean/edvr-unofficial-patch` or `3Shain/dxmt`.
The separate DXMT checkout is `/Users/sam/git/forks/dxmt`; verify repository/
remotes before acting there. `analysis/` is a separate private repository with
its own instructions and write/commit restrictions.

Approvals for the two diagnostic CI runs have been consumed. There is **no**
standing permission for another remote CI run, automatic retry, new push,
deployment, live-setting change, dependency installation, or Wine-process
termination. Ask before those actions. User wants only `d3d11.dll`, not an
installer or other runtime DLLs.

The existing graphics-only workflow is `.github/workflows/release.yml`,
manual `graphics_only=true`, Windows-job cap 20 minutes. Private-branch pushes
do not match its configured push triggers. Do not infer that a full build or
release is authorized.

Entire is enabled, but `.entire/settings.json` has `push_sessions=false` and
`telemetry=false`; do not upload sessions/checkpoints or bypass hooks. Consult
`entire agent-help` rather than inventing commands.

Keep material findings in the existing human-facing investigation document.
Do not repeat ruled-out flights without a new discriminating question.
