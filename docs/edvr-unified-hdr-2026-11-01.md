# HDR output inside EDVR (fix.hdr)

## RC6 private backport

The baseline description below refers to `28c50afb`. The combined
`private/rc6-hdr` branch now also contains the independent engine-hook and
HDR correction candidates documented below; combined CI/gameplay validation
is not yet established.

`private/rc6-hdr` starts at `v0.22.0-rc.6` and carries only the shared FP16
rendering/composition work and HDR display output, plus private-repository
policies. The HDR output implementation matches the supplied Polski source
snapshot. The shared-rendering patch preserves RC6's independent spatial
recovery cache rather than reverting that upstream behavior.

This branch targets vanilla DXMT. It does not import the custom MetalFX
Ext2/Ext3 interfaces, MetalFX modes, or the later camera allocation changes.
Imported HDR admission tests exercise RC6's existing AA modes instead.
HDR presentation support on vanilla DXMT is assumed from the owner's prior
test; this backport has not been gameplay-validated and is not a memory-leak fix.

Local checks: the 39 HDR output rule checks pass; the HDR output and
fullscreen-mode sources pass x64 and ARM64EC cross-syntax checks. A full
DLL build/GPU test remains outstanding. UI-quality tests require Windows;
broader local checks encounter missing generated temporal shader headers and
pre-existing MSVC/MinGW compatibility issues (including function-pointer
conversions in the device hook and a `near` macro collision in temporal tests).

The implementation and flight notes below describe the original HDR work,
not a validation receipt for this RC6 backport.

### RC6 engine-hook follow-up (`private/rc6-hdr-engine-hooks`)

The three 2026-11-05 22:34–22:40 flights accepted no temporal frames. Camera
installation succeeded, but the kinematic build hook failed nearby allocation,
leaving engine motion stood down and `engine-source-not-ready` at resolve.
The local candidate uses one allocation-granularity block per kinematic hook
under flat Wine: constrained `VirtualAlloc2`, then the original fixed search
only on ordinary failure/unavailability. CodeHook borrows the final page;
the relay owns the complete block. Unsafe placement/cleanup failures stop
that entry's retries. Native Windows/VR retain their old path.
The synthetic `near_alloc_test` passes under CrossOver; x64 engine-source and
x64/ARM64EC CodeHook/test syntax checks pass. ARM64EC engine-source checking
still encounters baseline `__readgsdword` incompatibility. No flight validation
or new CI result is claimed.

### HDR investigation (`private/rc6-hdr-output-fix`)

This branch starts at successful CI revision `28c50afb`, separately from
`private/rc6-hdr-engine-hooks`. Keep the owner's display preset, game gamma,
and vanilla DXMT unchanged; assume the HDR defect is in EDVR.

Apple's [color-space guidance](https://developer.apple.com/documentation/metal/using-color-spaces-to-display-hdr-content)
shows PQ BT.2020 content tagged `itur_2100_PQ` on an EDR-enabled Metal layer.
The XDR preset's **SDR** gamma 2.2 is not the HDR transfer function; 1600 nits
is a peak capability, not diffuse white.

For context, DXMT `647b0020` scales linear HDR light by current / potential EDR
headroom before re-encoding PQ. A local query returned 1.2 / 16 = 0.075,
outside the reported flight. This does not establish flight attenuation or
a DXMT defect, and is not authorization to compensate for it in EDVR.

Audit EDVR's tone replacement, Elite gamma/exposure inputs, scene-copy
261-to-500-nit unit conversion, and final output encoding. Verify both 1:1 and
0.75-scale routing without assuming the same copy shader executes. These
initial investigation notes preceded the local candidate described below.

#### Confirmed matching defects in the reported flights

All three 22:34–22:40 logs have zero `HDR replacement` messages, while the
final PQ encoder runs. The output draw uses copy PS `DED8796049C7BB4A`.
Its captured 268-byte DXBC has CRC `D6A846C8` and standard FNV-1a-64
`3C4D865B7031B44C`: exactly the HDR copy constants. However, `blobIs` calls
EDVR's `fnv1a64`, whose historical offset basis is `1469598103934665603`,
not the standard `14695981039346656037`. On those same bytes it produces
`DED8796049C7BB4A`, so the exact copy shader is rejected. Do not change the
global EDVR hash: existing shader identities depend on that convention.

The selected tone PS in these logs is `FEE777E92850B390`, captured as 2244
bytes with CRC `6BE69EF3`. The imported shader comments identify this CRC as
the no-bloom tone variant, but `hdrOutputReplacementPs` only supports the
3192-byte `D7E646E3` variant. Fixing the hash convention alone therefore will
not provide the required tone replacement for the observed scene path.
The bloom variant cannot simply be assigned to it: the replacement samples
bloom and must be adapted to the actual variant's input contract.

These establish incomplete EDVR HDR processing, not the numerical cause of
the user's gamma compensation. No claim is made yet about corrected image
brightness, and no live configuration or shader-capture file was modified.

#### Local correction candidate

HDR matching now uses standard FNV locally, leaving EDVR's global shader
identities intact. The captured tone and copy blobs both select their intended
replacement through the production matcher. The no-bloom tone variant is
compiled separately: scene `t1/s1`, constants `b2`, input `TEXCOORD1`, no
`t2/s2`, bloom blend, or post-blend clamp. Its display transform and luminance
constants remain the imported HDR ones.

Disassembly also exposed an exposure transcription error: the engine subtracts
`cb2[8].x + (cb2[8].z != 0 ? 1.4 : 0)` from `log2(8*v0.z)`, whereas the
imported helper added it. RenoDX's `tonemap_0xD7E646E3.ps_5_0.hlsl` confirms
the same subtraction for bloom. The shared helper now follows that operation.

Local checks: 54 portable rule/identity checks; 73 checks under CrossOver
hardware, including compiling/reflecting both tone variants and GPU execution
of the production exposure helper against the captured instruction arithmetic.
x64 and ARM64EC HDR source/test syntax checks pass. `build.bat` runs the same
Windows shader checks on WARP; full Windows CI has not run for this candidate.
There is no gameplay validation yet: corrected brightness, scale-path routing,
and the remaining imported display-transform assumptions are not established
by these tests. No DXMT, display preset, game gamma, or live settings changed.

### TEST-Present follow-up (local candidate)

Flight `edvr_gfx_20261105_234540.log` used `0693bebf`. It has only two
buffer resizes, both at startup; repeated resize-driven HDR recreation is
not supported by this flight. The passive Present census ended at 23:47:40
with `test=0`, before the menu reported lost focus at 23:50:08. That menu
message is not a complete focus-transition log.

Inspection found that the HDR transform and other frame work ran even on
`DXGI_PRESENT_TEST` status probes. The outer Present dispatcher now forwards
TEST calls without entering the frame-work function, preserving the original
arguments and HRESULT. Foreign chains forward without frame work or counters.
Ordinary owned Presents keep the existing frame path, even when their result
is occluded; this is not a new background-rendering policy.

`present status:` reports cumulative TEST/regular counts and their separate
occlusion counts, once initially and at most every 30 seconds thereafter.
There are at most 120 reports per DLL session; the final one says
`reporting-stopped=session-cap`. These counters do not touch frame state or GPU
resources. They can establish whether later app-switching uses TEST probes,
but this correction is not yet a demonstrated memory-leak or brightness fix.

Local verification: 61 portable HDR/dispatch checks and 80 checks under
CrossOver hardware pass; the Windows test builds for x64 and passes ARM64EC
syntax checking. The full device-hook syntax check remains blocked by the
same 30 MSVC/MinGW compatibility errors on baseline and candidate. No new CI
or gameplay validation is claimed.

#### Follow-up flight: TEST path not exercised

`edvr_gfx_20261106_135613.log` identifies `8bfba374` (build `6AEDDA04`).
Through the 14:10:15 status sample, TEST calls remain zero while ordinary
occluded returns reach 5,006. The reported memory-growth reproduction therefore
does not exercise the TEST guard. Ordinary occlusion is observed around
14:02–14:04, 14:06–14:07 and again near 14:10; only the two startup resizes
are recorded. FSR is selected from 13:59:53 onward.

From 14:02:14 to 14:04:14, DXGI local usage rises 6,250→7,070 MB and
Elite-tracked GPU allocations 3,838→4,626 MB, predominantly textures
(1,909→2,685 MB); the main CPU heap stays near 2,360 MB. Later GPU samples
both rise and fall, so this is not proof that every occluded interval leaks.
The screenshot shows Elite at 13.28 GB and yellow system memory pressure.
Those process/system metrics must not be added to the DXGI/engine counters.

The next inspection target is allocation/submission behavior during ordinary
occluded Presents, not TEST probes. `flatRuntimePresent` deliberately preserves
history on positive DXGI status returns; changing that to a per-frame reset
would revive a previously documented reset storm, not establish a leak fix.
Gamma remains at default; the user's familiar UI orange is a colour reference,
not a request to calibrate the UI separately. No leak owner is established.

#### Ownership inspection after the follow-up flight

The flight logs one FSR context creation at 13:59:53 (3024x1964 render cap
and output), not repeated context creation during occlusion. The inspected
`fsr3_engine.cpp` conversion/input targets use fixed slots and release old
targets before rebuilding on size changes. `flat_mono_resolve.cpp` similarly
reuses its matching-size images and clears them on recreation/reset. The HDR
encoder keeps one backbuffer/view set, with one scratch image only for its
fallback draw path.

The pinned FidelityFX DX11 backend (`9b04fa49abae5aa341c0007af13c8ba1b4ad9913`)
retains external resources across dispatches, but in fixed registration slots:
unregister resets the slot cursor; registering a different resource destroys
the old slot's views/reference before retaining the replacement. This is
bounded retention, not evidence of progressive accumulation.

Read-only inspection of vanilla DXMT's ordinary occlusion path confirms it
flushes/commits commands without presenting; completed command chunks have
their own reference cleanup. “Nothing is submitted or retired until the next
visible Present” is therefore not a supported explanation. Presentation
pacing differs, but that alone does not prove an allocation leak.

`mem_tick.cpp` reads twelve live-byte fields from Elite's renderer ledger;
`egpu_tex_mb` is one field, not EDVR-owned-texture accounting. The device
hook's existing creation counters measure allocations but not destruction.
The remaining discriminating measurement is texture-object lifetime:
created/destroyed/live/peak counts and estimated bytes, grouped by descriptor
and creation path, correlated with the existing occlusion counters. It must
report coverage failures and must not retain the textures it measures.
No missing release in the inspected FSR/HDR pools has been established, and
no rendering/lifetime correction is justified by this inspection alone.

#### Bundled-DXMT comparison, 2026-11-06 15:01 flight

The user reports reproducing the pressure/growth with CrossOver's original
DXMT. `edvr_gfx_20261106_150150.log` identifies the same EDVR `8bfba374`,
build `6AEDDA04`; it identifies DXMT but does not independently establish its
exact bundled version. The screenshot shows Elite at 12.03 GB and yellow
system memory pressure (11.56 GB compressed system-wide, not Elite alone).

At 15:03:50 / 15:05:50 / 15:06:50, respectively, reported local GPU usage was
5875 / 6533 / 7105 MB; Elite GPU accounting was 3454 / 4172 / 4836 MB; its
texture field was 1503 / 2063 / 2723 MB. The main CPU heap was 2199 / 2453 /
2453 MB. Thus the last minute includes another 660 MB of texture accounting
with a flat main-heap sample.

Crucially, cumulative ordinary occlusion was still zero at 15:06:21; the first
logged occluded result was at 15:06:47.703. Substantial growth therefore
preceded reported occlusion. TEST remained zero throughout the sampled
status lines. Only two startup resizes and one FSR context creation were
logged. Early AA mode changes make this an imperfect fixed-settings A/B,
but the nightly is not necessary for the user-observed symptom, and ordinary
occlusion is not necessary for the measured growth. Neither result identifies
the retaining owner or distinguishes all streaming/cache growth from leakage.

#### Texture-lifetime diagnostic candidate

The diagnostic adds a private-data `IUnknown` tag to successful texture results
from the existing `CreateTexture2D` hook on EDVR's device, in the flat profile.
The tag holds no texture reference. Its final release updates a process-lifetime
ledger, including releases on driver threads. The candidate deliberately pins
its DLL until process exit so late tag callbacks cannot enter unloaded code.

`texture lifetime` lines accompany the existing Present-status samples
(at least 30 seconds apart, at most 120 samples). Totals separate callers inside
EDVR's module image from external callers; these labels are not proof of the
ultimate retaining owner. Descriptor reports show at most eight groups per
sample, selected by largest absolute byte change and largest live byte totals.
The ledger holds 64 descriptor groups plus one overflow group per caller class.
At most 65,536 tags can be live/pending; skipped and failed attachments are
reported, and tracking can resume when tags are destroyed.

Bytes reuse the profiler's rough format/mip estimate, using the returned
descriptor (including HDR widening) and sample count. They are not physical
GPU allocation sizes. Textures predating the hook, swapchain buffers, and
creation through other entrypoints are outside coverage. A flat tracked-live
curve therefore narrows the search but does not rule out all D3D-object growth.
No rendering settings, temporal reset policy, or DXMT code is changed.

Local checks: 71 portable and 100 CrossOver checks passed, including a real
D3D texture/SRV lifetime test. The new production headers pass x64/ARM64EC
syntax checks; the complete device-hook translation unit still encounters
the same 30 baseline MinGW/MSVC incompatibility errors, with identical error
messages before and after this change. These are local checks, not a complete
Windows DLL build or gameplay confirmation.

#### Diagnostic flight, 2026-11-06 16:08

`edvr_gfx_20261106_160852.log` confirms diagnostic revision `8b001cc0`,
build `6AEDF5F7`. Tracking ran with zero attachment failures, cap skips, or
pending attachments at all twelve reporting snapshots. Screenshots show the
same Elite PID at 12.21, 12.94, and 12.81 GB with elevated system pressure;
these are not additive to the texture estimates.

Between 16:10:22 and 16:14:23, externally created tracked textures increased
from 5879 to 6701 live objects, and from 3282 to 4966 MiB estimated live bytes
(+822 objects, +1684 MiB). EDVR-image-created textures stayed at 49 live
objects / 1473 MiB: created/destroyed changed from 54/5 to 56/7, with the two
later creates balanced by destruction. This measures live-object growth,
not merely backing-memory growth while all tracked object lifetimes stay flat.

Identified growing external groups include shader-resource-only, mipmapped
1024x1024 BC4, BC5, and BC7 textures. For example, BC4_UNORM increased from
229 to 408 live objects. These descriptors are outside the HDR widening
predicate (which requires RGBA8 typeless and render-target/UAV binding).
The descriptor table filled, so some objects are aggregated into overflow
groups; their counts/bytes remain in the totals, but their individual
descriptors cannot be recovered from this report.

Creation origin does not identify the retaining owner. EDVR could retain
game-created resources/views, and legitimate streaming caches or runtime
retention remain alternatives. The next source target is the reference/
cache lifetime of externally created shader-resource textures, not another
speculative FSR/HDR working-target reset. No additional flight is needed
just to reproduce or confirm the growth already measured here.

Source follow-up: the inspected `FlatContextState` capture/restore path releases
all captured shader-stage SRV arrays, both on restore and destruction.
`flat_runtime.cpp`'s binding-view cache has six slots and resets an old owning
reference before replacing it. Foreground motion keeps current/previous draw
collections with frame rollover, rather than appending indefinitely; its
inspected retained views concern geometry/history. GUI/draw packet snapshots
have explicit arming/frame or byte/count limits and cleanup paths. These
checks found no growing material-texture container or demonstrated missing
release in those paths; they are not an exhaustive proof against EDVR retention.

A separate ambiguity is cache pressure: the same flight reports a roughly
28754 MB local GPU budget throughout, with only 8042 MB used (28%) at
16:14:28 despite the system-pressure screenshots. The local DXMT source's
`QueryVideoMemoryInfo` returns Metal's `recommendedMaxWorkingSetSize()` as
budget and `currentAllocatedSize()` as usage, for either memory segment.
This source inspection is not verification of the bundled binary's exact
implementation, nor evidence that Elite bases its texture eviction on that
API. A cache remaining below its perceived GPU budget while the whole machine
is under pressure remains a live alternative to lost references. No budget,
Wine-process, runtime, or live-setting change was made.

The user subsequently supplied an important control: the symptom does not
occur without EDVR. Treat EDVR-triggered retention/behavior as the leading
scope, not ordinary game caching as a sufficient explanation. The budget
observation above does not supersede that control.

The raw-reference follow-up inspected the HDR resource/view forwarding,
binding resolution, shader-resource scans, and explicit state capture.
`hdrOutputViewFormat` rejects BC formats before acquiring any reference and
uses `GetType`/`GetDesc`, not `QueryInterface`. Inspected raw `GetResource`
and `QueryInterface` success paths release their results or transfer them
into owning smart pointers. `binding_shadow.cpp` deliberately permits a
bounded leak if SEH interrupts resolution, but the flight contains no
`FAULT ABSORBED` or `bindingShadow.resolve` reports to support that path.
No ordinary-path unmatched reference was established by this pass.

A focused test in the existing HDR rig now exercises production
`FlatContextState` with 96 fresh BC4/BC5/BC7 textures, six shader stages,
upper SRV slots, normal restoration, repeated capture, and abandonment
through the destructor. Every iteration returns the tagged live-texture
count to zero. The local CrossOver run requesting WARP passed 104 checks;
71 portable checks and ARM64EC test syntax also passed. This does not test
Elite's live DXMT workload or establish balanced references in every hook.
Only tests and these notes changed; the installed diagnostic runtime remains
`8b001cc0`. The remaining gap is which caller retains the growing external
textures, which creation/destruction totals alone cannot identify.

#### HDR-output-off / 1.5x supersampling flight, 2026-11-06 16:56

The user reports HDR disabled for `edvr_gfx_20261106_165621.log`; revision
remains `8b001cc0` / `6AEDF5F7`. Startup resizes use format 28 (RGBA8 UNORM),
and no `hdr:` replacement/output-enable lines appear, consistent with that
setting. The engine's pre-tonemap HDR scene and the AA route named "flat hdr
route" are separate from the disabled HDR-output feature.

The scene changes to 4536x2946 at 16:58:04 for a 3024x1964 output, confirming
1.5x scaling on each axis (2.25x scene pixels). At 16:58:21 -> 17:00:21,
external tracked live textures grow from 6135 / 4990 MiB to 6661 / 6882 MiB;
EDVR's tracked set remains 37 / 1131 MiB through 17:03:52. Later external
byte samples fluctuate near that level, then fall near the end, rather than
forming an uninterrupted ramp. The screenshot shows Elite at 14.78 GB.
All attachment-failure and tag-cap-skip samples remain zero.

Importantly, all 74 route windows with a 4536x2946 HDR scene (16:58:07 to
17:04:14) report zero treated/reached/captured/backend frames: 6051 frames
declined before capture/backend execution. A startup FSR context exists, but
ongoing FSR treatment is not required for the growth in this interval. This
does not mean AA was off: its shared observation/engine-hook paths can remain
enabled before treatment admission. The user's planned AA-off comparison
therefore remains discriminating. Keep HDR and supersampling fixed for that
pair and start a fresh process; this flight's absolute memory footprint/rate
is not directly comparable with the earlier lower-resolution HDR-on flights.

#### AA-off control, 2026-11-06 17:35

The user reports no reproducible leak with AA disabled in the INI.
`edvr_gfx_20261106_173558.log` confirms `8b001cc0` / `6AEDF5F7` and
`flat runtime: mode=off`. Read-only inspection of the supplied live
`edvr-flat.ini` shows `[fix] temporal_aa = off` and `hdr = off`.
The log explicitly reports the UI-quality, sharp-weapons, and shield-ripple
paths waiting/not live because temporal AA is off; it contains no FSR context
creation or engine-motion reports.

Across roughly 11.5 minutes, tracked EDVR textures total three created and
three destroyed, with zero remaining at later snapshots. Compare the previous
AA-selected/HDR-output-off run's stable 37 textures / 1131 MiB. The control
also includes 5079 ordinary occluded Presents by the last status sample,
with zero TEST calls; occlusion alone does not reproduce the reported symptom.

External texture churn still occurs without the reported leak: at 17:40:28,
46476 creates minus 39711 destructions leaves 6765 live; at 17:43:28,
58953 minus 52166 leaves 6787 live, with estimated bytes almost unchanged
(6243614636 -> 6246416369). Later bytes rise and then fall, alongside eight
swapchain resizes across the session. Local GPU usage is 9416 MB at 17:43:59
and 8908 MB at 17:46:59; these are not perfectly matched fixed-resolution
samples against the earlier flight.

This narrows the leading scope to AA-enabled machinery, including work
before backend admission and dependent layers. It also corrects the earlier
interpretation: external live-texture growth alone is not leak-specific,
because substantial growth/churn also occurs in this no-symptom control.
The comparison must distinguish additional persistent AA working memory
from progressive retention; neither an AA-owned missing release nor a
particular hook is established yet. No live settings were changed by the agent.

#### AA-enabled pre-resolve path review, 2026-11-06

Source and existing-flight inspection, not a new gameplay test:

- `flatRuntimePresent` configures engine motion from AA selection before
  resolve preflight. `FlatRuntimeDrawScope` can call
  `engineVelocityFlatBeginDraw` and submit foreground captures before the
  later `treatHdr` rejection on `s.observing`. This is actual work, not merely
  an enabled setting: the HDR-off/AA-selected flight's final foreground SDK
  domain report records 29604 submitted GPU identity captures. The 17:04:13
  engine family report records 41078 substituted draws for
  VS `BBE58E40FE88EC80` / PS `DB3E8D20CF53FBC0`. That is the previously
  investigated decal family, not proof that it draws the missing glass signs.
- Correction to the earlier conversational explanation: the first recorded
  observation entry is `projection-preparation-refused` at 16:56:43.876,
  before the 16:58:04 switch to 4536x2946. Supersampling is not established
  as the cause of treatment refusal. "Observation" also does not mean all
  AA-related rendering is pass-through.
- Foreground capture count is workload, not retained-object count.
  `FlatForegroundMotion::beginFrame` rolls current draws into previous draws
  and advances history; capture passes use `FlatContextState` restoration.
  The flight's history reports peak at only 12 records / 4174272 logical
  position bytes, commonly 8 / 2782848, and show aged-record reclamation.
  `AnimatedVertexHistory` separately bounds its reusable position-buffer pool
  to 32 MiB and 128 pairs per history; a reused pair may be larger than the
  logical record count. These are not total physical GPU-memory measurements,
  but do not show a growing foreground-record container accounting for GiBs.
- The material-SRV reference in the engine overlay guard is held in
  `g_bound.gameSrv3` / `g_guardVerified.game` (ComPtr). `guardRestoreT3`
  releases the bound reference; `engineVelocityFlatFrameEnd` forgets the
  memo. `flatRuntimeBeforePresent` flushes substitutions before calling that
  frame-end cleanup. Normal inspected getter/restore paths balance their
  references; this does not prove every live DXMT interaction is correct.
- A distinct intentional retaining owner exists for game **buffers**:
  `notePrimaryBufferCreated` pins up to 16 dynamic structured buffers with
  stride 336 in `g_primaryMaps`, released on engine-motion stand-down.
  The flight reports zero overflow. This is not an identified owner of the
  growing material textures, nor an unbounded container; do not remove the
  pins without preserving the private-copy ownership contract.
- Sharp-weapons and shield-ripple report no applicable draws in the sampled
  late windows; ripple reports no displacement target allocated. The panel
  sizing hook reports zero supersampling-factor moves. Those logs weaken
  these particular mechanisms relative to the actively executing engine
  substitutions/copy path; they do not clear every UI-layer path.

No missing release or justified runtime correction was established. The
remaining distinction is AA's persistent resources and active pre-resolve
work versus genuine progressive retention, including driver backing storage
not measured by texture-object tags. No runtime code, settings, or artifacts
were changed for this review.

#### Prepared AA allocation-scope control and record-velocity isolation

The private control candidate adds `aa_memory_scope.h` and
`aa_memory_diagnostics.h`. Successful hooked buffer and Texture2D creations
are tagged without retaining the resource, using a GUID separate from the
existing texture-lifetime tag. The original all-origin texture measurement
remains intact. Allocations are attributed to engine-motion creation sites,
foreground capture/map preparation, resolver preflight/resolve (including
synchronous SDK calls), or other EDVR call sites. This is **allocation-scope
attribution, not proof of the current retaining owner**.

Buffer accounting uses full descriptor `ByteWidth`, so retained pool capacity
continues to count even when logical history records shrink. Texture bytes
remain estimates. No driver physical backing storage is measured. Unscoped
external buffers, pre-hook resources, swapchain images and other creation
entrypoints are excluded; allocations on other threads do not inherit the
calling thread's scope. Owner texture totals overlap the original texture
totals and must not be added to them. Each owner/kind has its own 65536-tag cap,
with observed, failure, skipped and pending counts. Reporting shares the
existing 30-second / 120-sample limit.

The same report snapshots the engine's up-to-16 pinned game buffers using
cached descriptor bytes, and gives cumulative execution counters for record
substitution attempts/successes, independent domain-marker attempts/successes,
foreground capture attempts, resolve preflights/attempts and completed
engine frame cleanup. Existing detailed foreground and backend reports remain
necessary to distinguish attempts, refusals and successful treatment.
Pinned-buffer bytes describe held references, not additional allocations;
they must not be blindly added to the allocation totals.

The control builds with `aa_memory::kIsolateEngineRecords = false`. Its
isolation derivative changes only this compile-time constant to true.
`engineVelocityConfigure` then takes the existing off/stand-down path in the
flat profile, without changing `fix.temporal_aa` or live settings. VR is
unchanged. This prevents the record emit/copy observers, pool-family
substitutions and their private copies from being enabled.

The independent domain-marker bracket in `engine_velocity.cpp` intentionally
remains available to foreground capture. Disabling that whole source module
would conflate the two paths. Existing engine-view accessors refuse missing
record-velocity inputs; consumers retain their normal refusal/fallback rules.
This is a diagnostic isolation, not a usable AA mode or a memory-leak fix.

For a future authorized flight, verify the DLL revision and
`isolate-engine-records=1`, `engine-active=0`, zero pinned engine buffers and
zero record substitutions. Then inspect whether foreground/domain work
actually continued. If it did not, disappearance of the symptom only
implicates the disabled group of work, not record copying alone. Keep HDR,
supersampling and gameplay comparable; record the glass-decals outcome
separately from memory. Flat owner totals versus physical allocation and the
ordinary texture lifetime curves distinguish persistent working memory from
progressive object growth, but stable owner counts cannot clear all driver
backing-storage behavior.

Local control checks: 75 portable checks and 116 CrossOver checks requesting
WARP passed, including dual-tag coexistence, final texture/buffer-view release,
full-capacity counting and thread-local scope restoration. ARM64EC test-TU
syntax passed. Device-hook syntax has the same 29 MinGW errors as the parent
under `-fms-extensions` / unlimited diagnostics (same error multiset).
Full engine/resolver/foreground compilation is blocked locally by the absent
generated `temporal_shader_bytecode.h`. These are not Windows DLL validation
or Elite/DXMT gameplay results. No remote CI or deployment was performed.

#### Record-isolation flight, 2026-11-06 23:05

After approved owner-fork push and graphics-only CI 42307373267 (passed in
8m41s), the user supplied `edvr_gfx_20261106_230550.log`. It identifies
`6b6b3978` / `6AEE5B22`, linked 22:56:34 UTC. The user reports AA not visibly
working, glass decals restored, and the memory problem still present.

All ten allocation reports confirm isolation active, engine records inactive,
zero pinned game buffers and zero record substitutions. Resolver attempts
remain zero; preflight attempts reach 11762. HDR-route reports decline with
`engine-source-not-ready`, explaining the absent treatment without requiring
a visual guess. Initial mode is `on` (TAA); at 23:09:10–14 the user cycles
DLSS, FSR, XeSS, off and back to on. Scene/output reports show 3024x1964,
not the previous 4536x2946 supersampled scene. This is not a fixed-settings
rate comparison against the earlier FSR/supersampled flight.

Before that mode cycle, external estimated live texture bytes rise from
2402789687 at 23:06:50 to 4120714111 at 23:08:50 (2292 -> 3930 MiB).
Foreground capture and domain-marker attempts are both zero throughout
those samples. The brief later foreground activity submits 372 domain
markers and makes 22 buffers, all 22 destroyed by 23:09:20. No foreground
textures are recorded. A further foreground-disable flight is not needed
just to establish that growth can occur without foreground capture.

At 23:09:20–23:10:20 all owner totals are stable: engine/domain helper one
texture / 95026176 estimated bytes plus a 16-byte buffer; resolver eight
textures / 172234944 bytes plus a 336-byte buffer; other EDVR four textures /
55691664 bytes plus four buffers / 976 bytes. Foreground live bytes are zero.
The surviving engine-labelled allocations belong to the independently
enabled domain path, not evidence that record isolation failed.
Attachment failures, cap skips and pending attachments are zero. Local GPU
usage rises 3520 -> 7530 MB over the periodic samples; the supplied screenshot
shows system pressure/compression but its `wine` filter does not show Elite's
per-process usage.

The glass regression is now implicated in the disabled record-motion group;
the exact responsible shader/draw is still unproven. The memory symptom does
not require that group, successful resolve, or sustained foreground capture.
The next active shared-path target is projection/buffer tracking and map
bounce: late five-second windows still perform roughly 70000 bounced maps
and 166 MB of flush traffic, with zero open mappings at Present. Those are
work/traffic counters, not retained-byte measurements. Owner-created resource
totals do not show a runaway pool, but do not clear retained game references,
CPU-side bookkeeping or driver backing-storage behavior.

#### Memory investigation: current conclusions and next step

**Quick recap as of the 23:05 isolation flight:**

| Comparison / observation | What it tells us |
| --- | --- |
| User reports no problem without EDVR, and could not reproduce with `fix.temporal_aa = off` | AA-enabled EDVR behavior remains the leading scope; external texture creation does not absolve EDVR of retaining or indirectly affecting those resources. |
| HDR-output-off run still reproduced | HDR output is not required for the reported memory problem. |
| Record-isolation build restored glass decals, but memory trouble remained | The decal regression is implicated in the disabled engine-record group. The memory symptom does not require that group. Treat the two symptoms separately. |
| Isolation log records zero substitutions, zero pinned engine buffers and zero resolver attempts | The isolation actually executed as intended; this was not just a setting that failed to take effect. |
| Texture growth preceded any foreground capture in that flight | Another foreground-off flight is not needed merely to establish that growth can occur without capture. |
| Measured EDVR allocations stayed bounded while external live textures increased | No growing EDVR-created pool has been identified. This does **not** identify who retains game resources or rule out CPU storage / driver backing-storage growth. |

The exact leak/retention mechanism remains **unidentified**. Live texture growth
alone is not proof of leaked references: the no-symptom AA-off control also
loaded substantial texture data. The runs differ in AA mode, scene resolution
and activity, so their absolute footprints and growth rates are not a clean
one-variable comparison. The isolation build is a diagnostic, not a usable AA
mode or a confirmed fix; do not expect it to provide correct temporal AA.

**Next: source inspection of projection tracking and Map/Unmap bounce.**
No additional flight is needed before this inspection. Follow ownership through:

1. **Registration:** which game-buffer references and CPU shadow/history storage
   are acquired when a buffer enters tracking.
2. **Map/Unmap completion:** whether success, refusal and failure paths all release
   temporary ownership.
3. **Replacement and retirement:** whether old identities, references or storage
   survive after the game replaces a buffer.
4. **AA stand-down:** what AA-off releases that stays resident while AA is
   selected but treatment is refused.

Zero open mappings at Present argues against simply leaving maps open; it does
not account for references or storage retained after completed mappings. The
roughly 166 MB per five seconds is copy traffic, **not a measured leak rate**.

If a concrete lifetime defect is found, add a focused regression test and fix
that defect. If source inspection remains inconclusive, the next candidate is
a temporary map-bounce bypass: preserve the game's original Map/Unmap calls,
and safely refuse treatment that needs unavailable tracking. This candidate
has **not been implemented or tested**. Further remote CI and deployment need
separate approval; no speculative signage fix, blanket cache reset or permanent
INI switch is part of this step.

#### Focused buffer-interception source review after the isolation flight

Reviewed the working tree at `86ae14a4`, following the real Map/Unmap hooks
in `vscreen.cpp`, the adapters in `flat_runtime.cpp`, `flat_map_bounce.h`,
and the production projection runtime, binding plans and shadow bank.

- **Registration:** `FlatProjectionRuntime::track` accepts only constant
  buffers, 16-byte aligned and at most 65536 bytes. It retains them through
  ComPtr in a 64-entry table. Rejected QueryInterface results are local ComPtrs.
  The CPU shadow bank allocates its 64 x 65536-byte payload once (4 MiB);
  full-write counters do not represent accumulating allocations.
- **Map completion:** bounce uses eight reusable slots, each containing a
  64 KiB payload plus a 64 KiB independent seed/baseline (about 1 MiB total,
  plus metadata). An installed mapping retains its resource and context.
  `beginUnmap` copies to the original driver pointer and returns an RAII
  lease; `finish` releases both references. The hooked Unmap finishes that
  lease before the real Unmap on the normal, foreign-context and internal
  paths. Unmap cleanup is not conditional on AA remaining enabled.
- **Abnormal mappings:** remapping releases an abandoned slot; wrong-context
  Unmap preserves it for the matching context and trips the diagnostic;
  overdue mappings trip at Present rather than invalidating a pointer still
  owned by the game. This retention is bounded to eight slots. The flight's
  zero open-at-Present/trip counters do not support that case here.
- **Replacement:** unpromoted, unmapped, nonpending tracked buffers are
  evicted under capacity pressure. `discard` releases their shadow identity,
  private-buffer state and original ComPtr. Plan retirement removes logical
  pins so entries can become evictable.
- **Retention caveat:** `retirePlan` marks a plan unused and demotes its
  references, but does not immediately clear the plan's saved original /
  replacement ComPtrs. Those can remain until plan reinitialization or
  runtime destruction. This is bounded by the fixed plan arrays, not a
  demonstrated unbounded leak. The isolation flight's projection-readiness
  report at the brief AA-off transition shows zero prepared projections;
  it does not establish that these saved plans caused the growth. The new
  allocation diagnostic's preflight counter measures resolver preflights,
  not projection-plan creation. No speculative plan-lifetime change was made.
- **Stand-down:** leaving the watched work tier or resizing/stopping deletes
  the projection runtime and releases its tables, plans and shadow storage.
  The process-lived bounce core deliberately survives with its fixed slot
  storage so any outstanding Unmap can still finish.

Local checks: the existing native `flat_map_bounce_test --self-test` passed
with zero failures, including reference balance, mode changes, wrong-context
handling, remapping and randomized transactions. Both production projection
implementation files passed x64 MinGW syntax checks.

The full existing projection-test header pulls in the missing generated
`temporal_shader_bytecode.h`, so it could not be built locally. Instead, a
focused temporary driver in `build/aa-buffer-investigation/projection_test.cpp`
linked the actual `flat_projection_runtime.cpp` and `flat_projection_scope.cpp`.
It created 576 fresh 64 KiB dynamic constant buffers, registered full writes,
performed real discard Map/Unmap transactions, dropped caller references,
and checked lifetime tags through table eviction, reset/reinitialization and
runtime destruction. All 2312 checks passed: the unpromoted table retained at
most 64 buffers / 4 MiB and all 576 resource tags were ultimately destroyed.
This probe does not exercise active cached projection plans or Elite's hooked
draw pipeline. It ran under CrossOver; the runtime warned that requested
WARP driver type 5 was unsupported while device creation succeeded, so this
must not be described as a true WARP or Windows validation result.

**Conclusion:** no missing release or unbounded shadow allocation was found
in these inspected normal paths. This does not clear interception behavior:
bounce substitutes the pointer returned to the game and seeds it from the
previous shadow (or zeros), then copies the full buffer back before Unmap.
Balanced references alone cannot prove that interaction harmless, or account
for physical driver backing allocations.

If another isolation candidate is needed, bypass only bounce pointer
substitution/writeback, retaining the game's original Map/Unmap calls and
projection observation where valid. The existing Off path still finishes
outstanding leases. Do not conflate that test with disabling all projection
tracking or foreground capture. No bypass, runtime fix, CI run, installation
or live-settings change was made during this review.

## Original status

State: implemented, flown once, DID NOT RUN, cause found and fixed, rebuilt.
`claude/hdr-unified` on `e785c183` (the installed MFX4 / UI75 candidate),
local only.

The 2026-11-01 18:20 flight wrote ZERO `hdr:` lines. Cause: the flat profile's
key gate (`src/common/flat_profile_keys.h`) did not name `fix.hdr`, so
`getString` answered `"off"` before reading the file and the feature was never
asked to run. Fixed in `1baf49d0`; "Why the first flight was silent" below.

What it does: the five jobs the separate EDHDR proxy did, at hook sites EDVR
already owns, so no new interception of any kind. `src/d3d11/hdr_output.{h,cpp}`
plus EDHDR's four HLSL files verbatim in `src/d3d11/hdr_output_shaders.h`.
Everything is gated on `fix.hdr`, which is off.

What is NOT claimed, and this is the important line: this will NOT reach the
SDR baseline's 10.73 W + 0.2 W. The 2026-10 power work showed the extra GPU
work is the FP16 *composition* domain -- every full-resolution surface EDVR
copies and composites at 8 bytes a pixel instead of 4 -- not the proxy chain.
Removing the chain removes CPU cost (two sets of hooks on one context, one
fewer DLL) and some GPU cost (the output transform is one in-place dispatch,
16 B/px, instead of a copy and a draw at 32, and the game's clears are issued
once instead of twice). It does not remove the 8 B/px surfaces, because
display-referred HDR needs them. Reaching ~11 W needs a 10-bit encoded
composition domain, which moves where the game's own 2D interface blends.
That is the next experiment and it is not in this branch.

Temporary keys (arc debt, to retire when the arc closes):
  * `fix.hdr` = off | on. Should become a menu row on the flat panel's
    Performance tab once it has flown; it has no row today.
  * `advanced.hdr_encode` = pq | linear. An A/B for a runtime that refuses
    the PQ colour space. The fallback is automatic, so this only forces
    linear deliberately. Ship the winner and remove the key.

Deliberate deviation from `shader_swap.h` ("Fixed HLSL belongs in the build"):
the four shaders compile through `d3dcompiler_47` at device attach, not in
`tools/temporal_shader_build`. Four shaders at attach is not the first-Present
storm that rule was made for, and keeping EDHDR's text unedited is what makes
the port checkable against it. Moving them into the build tool is one Variant
entry each and is the production follow-up.

Next: fly it. `fix.hdr = on` in `edvr-flat.ini`, EDHDR OUT of the chain
(install as `d3d11.dll`, no `CHAIN_D3D11`), MFX4 / UI75 / 2336x1510, against
a22's `14.66 W @ 1301.1 MHz, 15.28 ms` (19,881 cycles/frame). What the log
must say is in "Reading a flight" below.

Ruled out so far:
  * ruled out: the hook sites, the shader identities, the widening predicate
    and the output transform as causes of the 2026-11-01 silent flight,
    because none of them ran at all: the config gate turned the feature off
    before the first hook was installed. Nothing is yet known about whether
    they work.
  * ruled out: porting EDHDR's clone chain, because in-place widening is what
    the oracle (RenoDX) actually does, needs no bind-time heal, no replayed
    clears and no 512-entry map, and fixes the clone chain's own documented
    gap (CopyResource between the game's originals was never mirrored).
  * ruled out: presenting R10G10B10A2 with no composition change, because
    UNORM clamps at 1.0 and the tone shader's ceiling is about 2.27.

## Why the first flight was silent (2026-11-01)

The candidate was installed as the primary `d3d11.dll` with `hdr = on` in
`edvr-flat.ini`, and the log carried no `hdr:` line of any kind: no banner, no
swapchain line, no `created wide`, no refusal. Output stayed DXGI format 28.

In the flat profile, `Config::getString` consults
`runtimeProfileAllowsKey` first, which calls `flatProfileReadsKey`, and for any
key that list does not name it returns the string `"off"` WITHOUT LOOKING AT
THE FILE. `fix.hdr` was not on the list. So `hdrOutputStartup` read `"off"`,
took its early return, and every hook site -- each gated on `hdrOutputOn()` --
stayed dark. Not even the module's own "that is not a value this reads" line
could appear, because the gate returns exactly the spelling that means off.

This is the failure mode the module's own header warns about in a different
place: a path that does nothing and says nothing. The feature's code was never
in question, and nothing is yet known about whether it works.

`fix.temporal_aa` is not subject to that gate -- `requestedTemporalMode()`
reads the parsed map directly -- which is exactly why mfx4 and `ui_quality`
worked from the same file that could not turn HDR on, and why the setup looked
healthy.

The fix names `fix.hdr` and `advanced.hdr_encode` in the gate. Unlisted,
`advanced.hdr_encode` was the milder of the two: `"off"` is not `"linear"`, so
the domain stayed PQ and only the A/B was unreachable. `hdr_output_test` now
reads the gate's own text and fails when either name is missing -- verified by
removing the entry, which makes the rig fail that check and exit 1. A C++
assertion was not available: the rules header is deliberately Windows-free and
the gate is not, so the check is on the file's text, as `sharp_weapons_test`
and `aa_compare_test` already do for their keys.

Two things the same flight showed, neither a cause:

  * `fix.hdr = on` written under `[fix]` parses as `fix.fix.hdr`, because the
    parser always prefixes the section. The key is spelled `hdr` there, as
    `edvr.ini` documents it. The audit line naming `fix.fix.hdr` is what a
    doubled prefix looks like in a log.
  * the VM's build stamped `version unknown (build 6AE758CB)`, because
    `build.bat`'s own `git describe` hit git's dubious-ownership refusal when
    run as SYSTEM through `prlctl exec`. `git config --global --add
    safe.directory C:/edvr-mfx-finish` inside the VM fixes it. Without it no
    build from that VM can be checked by `edvr_log.py --expect-build HEAD`,
    which is the project's first question about any flight log.

## What the frame looks like

Unchanged from the chained setup, which is the point: the substituted tone
shader writes display-referred gamma-2.2 BT.709 with 1.0 = 261 content nits
into a target EDVR created wide; EDVR takes that as its resolve input,
upscales with MetalFX 4, composites the HUD layer, and hands the result to the
game's final copy draw, whose substituted shader rescales 261-nit units into
500-nit units; the game then draws its 2D interface onto the backbuffer and
EDVR draws its menu; and only then, immediately before the real Present, the
whole backbuffer is encoded to PQ BT.2020 (graphics white 500 nits) and the
swapchain is tagged `RGB_FULL_G2084_NONE_P2020`.

The encode has to be last. The game's interface is drawn after the final copy,
so an encode anywhere earlier would present it in the wrong domain.

## Where it differs from EDHDR, and what to watch for

1. **Widening in place, not cloning.** Elite's `R8G8B8A8_TYPELESS` targets
   with a render-target or unordered-access bind are *created*
   `R16G16B16A16_TYPELESS`. Format and bind flags only, no size test (the real
   tone target is 2160x1396 against a 2336x1510 display, and a size rule is
   exactly what used to miss it). Excluded: multisample, arrays, shared,
   staging and CPU-visible resources, and every create that comes from EDVR
   itself.
2. **An inexact view format is mapped, not refused.** EDHDR could fall back to
   the 8-bit original for a view it could not map, because it kept one. In
   place there is no original, and refusing a view create is fatal in Elite,
   so a `UINT` or `SNORM` view over a widened target is built as
   `r16g16b16a16_float` and said out loud (capped at 8 lines). If any such
   line appears in a flight log, that pass reads something EDHDR let it read
   as 8-bit, and it is the first thing to look at for a visual difference.
3. **The output transform is one dispatch.** EDVR creates the swapchain, so it
   asks for `DXGI_USAGE_UNORDERED_ACCESS` and encodes the backbuffer in place.
   If DXMT does not honour the usage, the UAV create fails and the scratch
   copy and fullscreen draw EDHDR used take over; the log says which ran.
4. **The substituted shaders keep the game's hashes.** The replacement object
   is handed to the game, but EDVR registers the ORIGINAL bytecode and hash
   for it, so the tonemap admission, the copy pair and the HUD layer's tone
   re-issue see the draws they already know. The chained setup needed a
   special case for EDHDR's copy shader hash (`kEdhdrCopyPs`); this does not.
5. **No HDR metadata, and no display query.** As in EDHDR: the nits constants
   are the oracle's (peak 1589, diffuse 261, graphics 500) and nothing asks
   the display what it can do. DXMT's `SetHDRMetaData` is a no-op anyway.

## Reading a flight

With `fix.hdr = on`, the log must carry, in this order:

  * `hdr: on.` once, naming the domain it will ask for;
  * `hdr: swapchain asked for format 87, created as 10 with unordered access`;
  * `hdr: shaders ready at device attach` -- and it must say *a compute
    dispatch in place*, not *a fullscreen draw*, or finding #3 above applies;
  * `hdr: target #1 created wide: 2160x1396 ...` and up to seven more;
  * `hdr: the game's tone pixel shader (len 3192 crc 0xD7E646E3) was handed the
    HDR replacement`, and the same for the final copy (len 268, 0xD6A846C8).
    Both must appear. Only the tone line means the copy shader changed and the
    output stage is reading the wrong unit;
  * `hdr: output domain PQ BT.2020 ... in place=1`;
  * `hdr: output transform #1 applied 2336x1510 in place`, then silence.

Absent lines are the diagnosis. No `created wide` lines means the widening
predicate missed, and the HDR is clamped to SDR even though the output stage
ran. No `output domain` line means the colour space was never applied.

## Validation done, and not done

Full `build.bat --wait-lock` in the Windows 11 ARM64 VM at `8e8d18f0`:

  * **all 35 Python gates passed**, including `check_config_contract.py` (238
    keys, code, ini and log messages agreeing) and `hook_signatures.py audit`
    over `src\` (788 files). They pass only with the x64 Python under
    `%LOCALAPPDATA%\EDVR\python313-x64` first on PATH: the ARM64 Python the
    VM answers `where python` with cannot install `capstone`, which has no
    Windows-ARM64 wheel, and `hook_signatures.py --self-test` needs it. The
    same run needs `EDVR_NGX_SDK` and `EDVR_FFX_DX11` pointed at that folder,
    or the DLL comes out 8 MB smaller with no DLSS and no FSR3.
  * `build\d3d11.dll` built, 30,205,952 bytes.
  * 177 rigs were discovered, `hdr_output_test` among them, and the pool
    stopped at `installer_test` after 31 of 182 jobs -- as it does in this VM
    at every recent commit, because that job waits for a window to appear and
    the session it runs in has no interactive desktop ("no window was shown,
    no console opened and the foreground never moved ... in 83 s"). So there
    is **no `full_build_receipt.json`** and this is not a full-build-validated
    binary, exactly as the last three candidates were not.
  * eleven rigs were then run individually (`build.bat --rig <label>` with the
    toolchain environment set by hand, which is what the pool does), and all
    eleven pass: `hdr_output_test` (28 checks), `config_test`, `vtable_test`,
    `ui_quality_test`, `flat_mono_resolve_test`, `gpu_live_hook_test`,
    `native_device_test`, `format_support_test`, `ui_layer_device_test`,
    `gpu_census_test` (262 checks) and `flat_temporal_test`. The first is the
    feature's own; the rest are the device-hook, config and flat-path rigs this
    change could have broken.
  * `hdr_output_test` also builds and passes with clang++ on the host, which is
    what the Windows-free rules header is for.

On the Mac: `gen_settings_schema.py --check` (75 exposed settings, 3 needing a
restart), and the new module parses clean under mingw g++ with the real
Windows headers -- a syntax check and nothing more.

**Not validated:** the image. No frame from this build has been looked at. No
existing GPU rig covers the feature either -- the rigs test the resolver, the
HUD layer and the temporal pass, none of which this touches -- so what the
rigs would have proved is only that nothing else broke, and `installer_test`
stopped the pool before most of them ran.
