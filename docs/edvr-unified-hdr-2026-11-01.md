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
