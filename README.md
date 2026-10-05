# shadps4-open-test

Freely redistributable synthetic PS4 guest workloads for developing and regression-testing
[shadPS4](https://github.com/shadps4-emu/shadPS4), built with OpenOrbis + OpenGNM and
without an official Sony SDK, extracted Sony PRX/sysmodules, commercial-game assets, or
commercial-game shaders.

## Stage 0 prototype

The first target is `gpu_solid_rt`: a deliberately small GNM raster/readback test.

It:

1. creates a 64x64 linear `R8G8B8A8_UNORM` render target in CPU+GPU-readable direct memory;
2. clears the guest backing memory to zero;
3. runs a PS4 GFX7 vertex/pixel shader pair built from GLSL authored in this repository;
4. draws one oversized fullscreen triangle whose fragment shader writes exact opaque white;
5. uses an EOP event with color-buffer flush;
6. reads sixteen interior pixels back from guest CPU memory;
7. emits a machine-readable result marker:

```text
SHADTEST name=gpu_solid_rt status=PASS samples=16 expected=ffffffff
```

This is intentionally stronger than the current OpenGNM `hardware_smoke`: that smoke validates
submit/EOP, but its visible status buffer is filled by the CPU and its GNM draw has zero
vertices.

The test uses current OpenGNM APIs directly. `freegnm-examples` was used as a reference for
known-good pipeline structure, but neither freegnm nor its shared compatibility layer is a
stage-0 build/runtime dependency.

## Important oracle caveat

Current shadPS4 defaults disable GPU readbacks and linear-image readback. Therefore guest CPU
verification of a GPU-written render target is not a neutral renderer-only oracle.

By default, `scripts/run-test.py` creates an isolated shadPS4 `XDG_DATA_HOME` with:

```toml
[GPU]
readbacksMode = 2
readbackLinearImages = true
```

So `gpu_solid_rt` deliberately validates a combined path:

```text
shader/raster/render-target correctness
             +
linear GPU -> guest CPU readback
```

A later VideoOut/screenshot test should give an independent raster/presentation oracle, and a
later dedicated test should isolate coherence/readback behavior.

## Current status

The repository scaffold, pinned dependency bootstrap, first guest test and host runner are
implemented. The design and current upstream findings are recorded in
[docs/research-2026-10-05.md](docs/research-2026-10-05.md).

The prototype has **not yet been fully built and executed on a PS4 or shadPS4 GPU host from the
research environment that created it**. The first real Linux GPU run is therefore a validation
checkpoint; build/runtime failures should be fixed from evidence rather than hidden behind
additional framework code.

## Dependencies

Pinned build inputs are recorded in [deps.lock](deps.lock).

Stage-0 build dependencies:

- OpenOrbis PS4 Toolchain v0.5.4: guest compiler/sysroot/link/runtime inputs;
- OpenGNM: GNM implementation/API, MIT;
- opengnm-psbc: SPIR-V -> PS4 GFX7 shader compiler, MIT project with vendored Mesa code;
- SPIRV-Headers and Vulkan-Headers: host-side psbc build inputs;
- `glslc`: GLSL -> SPIR-V.

`freegnm-examples` is pinned only as a research/reference baseline and is not built by the
bootstrap script.

No extracted Sony runtime module is copied into this repository.

## Fresh Ubuntu build

Install the small test-suite-side host tool set first. For Ubuntu this is approximately:

```bash
sudo apt update
sudo apt install -y git curl ca-certificates make clang lld llvm glslc \
    python3 python3-mako
```

Then:

```bash
git clone https://github.com/kaaburgh/shadps4-open-test.git
cd shadps4-open-test

bash scripts/bootstrap-deps.sh
bash scripts/build-test.sh gpu_solid_rt
```

`bootstrap-deps.sh` downloads the official OpenOrbis v0.5.4 Linux toolchain archive, verifies
its published SHA-256, checks out the source dependencies at exact revisions, builds
`opengnm-psbc`, and builds the OpenGNM Orbis static library under `.deps/`.

The expected guest output is:

```text
out/gpu_solid_rt/
├── gpu_solid_rt.elf
└── assets/
    ├── fullscreen.vert.sb
    └── solid.frag.sb
```

## Build current shadPS4 on Ubuntu

Current upstream recommends Clang 19:

```bash
git clone --recursive https://github.com/shadps4-emu/shadPS4.git
cd shadPS4
cmake -S . -B build/ -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19
cmake --build build --parallel "$(nproc)"
```

See the upstream Linux build document for the full distro package list.

## Run

With a Vulkan-capable X11 or Wayland session:

```bash
cd /path/to/shadps4-open-test

SHADPS4=/path/to/shadPS4/build/shadps4 \
    bash scripts/run-test.sh gpu_solid_rt
```

The host runner:

- uses an isolated shadPS4 config unless `--use-host-config` is passed;
- scans combined shadPS4 output for `SHADTEST`;
- saves `out/gpu_solid_rt/run.log`;
- returns 0 for guest PASS, 1 for guest FAIL, and 2 for infrastructure/no-marker failure;
- stops shadPS4 after a result marker if the frontend remains alive.

shadPS4's own host-process exit status is not used as the semantic guest result.

## Why raw ELF

Current shadPS4 can boot a PS4 ELF directly. In the direct-ELF path, the executable's parent
directory is mounted as `/app0`, so generated shader assets beside the ELF are available at
`/app0/assets/...`.

That removes package metadata, installation, icons, SFO/GP4 construction and package-only
support files from the first proof.

OpenOrbis PKG samples often include `libc.prx` and `libSceFios2.prx`, but current OpenOrbis
contains source for these stub/empty modules itself. They are not evidence that an extracted
Sony SDK/runtime is required. Raw ELF is preferred here because it is simpler.

## Headless / Vast.ai

Current shadPS4 has renderer-side headless concepts, but the current CLI/frontend does not
provide a practical displayless `--headless` guest-run path. A recent upstream synthetic-ELF
report reaches the loader and then fails at SDL video initialization on a host with no display.

For now, the runner refuses a Linux launch with neither `DISPLAY` nor `WAYLAND_DISPLAY`
unless `--allow-no-display` is explicitly supplied. Do not assume Xvfb is sufficient for a
Vulkan GPU run until it has been measured.

A small real shadPS4 headless frontend path is a separate follow-up task, not a prerequisite for
validating the guest build itself.

## Result contract

Each test owns a semantic terminal result:

```text
SHADTEST name=<test> status=PASS ...
SHADTEST name=<test> status=FAIL reason=<reason> ...
```

The marker may be mirrored through more than one guest logging path; the host treats the first
matching terminal marker as authoritative.

Future tests should remain small and independent. When a bug is first found in a commercial
game, the preferred long-term regression artifact is a minimized open guest reproducer rather
than a permanent dependency on the game.

## License

Code authored in this repository is MIT licensed. Third-party code remains under its upstream
license; see [THIRD_PARTY.md](THIRD_PARTY.md).
