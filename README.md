# shadps4-open-test

Freely redistributable synthetic PS4 guest workloads for developing and regression-testing
[shadPS4](https://github.com/shadps4-emu/shadPS4), built with OpenOrbis + OpenGNM and
without Sony SDK files, commercial-game assets, extracted PRX/sysmodules, or proprietary
game shaders.

## Stage 0 prototype

The first target is `gpu_solid_rt`: a deliberately small GNM raster test.

It:

1. creates a linear `R8G8B8A8_SRGB` render target in CPU+GPU-readable direct memory;
2. clears it to black;
3. runs a PS4 GFX7 vertex/pixel shader pair built from GLSL;
4. draws one oversized fullscreen triangle whose fragment shader writes opaque white;
5. uses an EOP event with color-buffer flush;
6. reads selected pixels back from guest CPU memory;
7. prints one machine-readable result line:

```text
SHADTEST name=gpu_solid_rt status=PASS samples=16 expected=ffffffff
```

This avoids using a screenshot as the primary oracle. A later host-side screenshot can still
be kept as a diagnostic artifact.

The prototype is intentionally based on the small, MIT-licensed compatibility/common layer
used by `PS4-OpenGNM/freegnm-examples`, while the actual test source and shaders live here.

## Current status

The repository scaffold and the first test source are implemented. The design and current
upstream findings are recorded in [docs/research-2026-10-05.md](docs/research-2026-10-05.md).

The code has **not yet been executed on a PS4 or shadPS4 GPU host from this ChatGPT run**.
Accordingly, the first real run is a validation checkpoint, not something this repository
pretends has already passed. In particular, it must confirm shadPS4's handling of a linear
GNM color render target and guest CPU visibility after the EOP flush.

## Dependency policy

Pinned upstream inputs are recorded in [deps.lock](deps.lock).

- OpenOrbis PS4 Toolchain: GPL-3.0, used as the external compiler/sysroot/toolchain.
- OpenGNM: MIT.
- opengnm-psbc: MIT (with vendored Mesa code under its upstream licenses).
- freegnm C compatibility headers: MIT.
- freegnm-examples shared support code: MIT.

No Sony runtime modules are copied into this repository. The preferred shadPS4 workflow is
a raw ELF plus sibling `assets/`; no PKG is built.

## Intended workflow

On a fresh Ubuntu GPU machine:

```bash
git clone https://github.com/kaaburgh/shadps4-open-test.git
cd shadps4-open-test

./scripts/bootstrap-deps.sh
./scripts/build-test.sh gpu_solid_rt

SHADPS4=/path/to/shadps4 ./scripts/run-test.sh gpu_solid_rt
```

`bootstrap-deps.sh` pins source dependencies and installs the official OpenOrbis v0.5.4
toolchain archive locally under `.deps/`. System packages are deliberately not installed
by the script; the required Ubuntu packages are listed in the research notes.

## Why raw ELF

Current shadPS4 can boot a PS4 ELF directly. For a direct ELF launch, shadPS4 mounts the
executable's parent directory as `/app0`, so shader assets beside the ELF can be opened as
`/app0/assets/...`.

That removes PKG metadata and, more importantly, removes the usual OpenOrbis homebrew PKG
step that copies runtime files such as `libc.prx` and `libSceFios2.prx`. Those files are
not part of this test-suite workflow.

## Result contract

Tests should emit exactly one terminal marker:

```text
SHADTEST name=<test> status=PASS ...
SHADTEST name=<test> status=FAIL reason=<reason> ...
```

The host runner treats the marker as authoritative. shadPS4's own process exit code is not
used as the guest PASS/FAIL oracle.

Future tests should remain small and independent. A game-specific bug should, where
practical, be reduced to a new synthetic test rather than making this repository depend on
that game forever.

## License

Code authored in this repository is MIT licensed. Third-party code remains under its
upstream license; see [THIRD_PARTY.md](THIRD_PARTY.md).
