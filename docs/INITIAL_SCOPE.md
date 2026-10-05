# Initial scope

Recorded: 2026-10-05

## Goal

Create a freely redistributable synthetic/homebrew workload for shadPS4 development
that can be cloned onto disposable Linux GPU machines and exercised without a commercial
PS4 game dump.

The desired end-to-end chain is:

```text
open source guest source
  -> OpenOrbis + OpenGNM build
  -> PS4 guest ELF
  -> shadPS4
  -> real GNM/GPU work
  -> deterministic machine-readable oracle
```

## Constraints

- No official Sony SDK.
- No extracted Sony PRX/sysmodules.
- No commercial game executable, shader, texture, key, license, RIF or asset.
- Prefer raw ELF over PKG for the first stage.
- Source-first repository: generated binaries are reproducible outputs, not required inputs.
- Start with one small test rather than a general test framework.
- Preserve a path to adding one small reproducer per future shadPS4 bug.

## Questions to answer

1. Can current OpenGNM applications be built with current OpenOrbis only?
2. What host and guest runtime dependencies are actually required?
3. Do current samples hide proprietary runtime inputs?
4. Which generated artifacts are reasonable to redistribute?
5. How deterministic can a GPU oracle be?
6. Can current shadPS4 run this displayless/headless on Linux cloud GPUs?
7. What is the right result channel: exit code, guest log, image, marker, or a combination?
8. Does an equivalent maintained guest-side conformance suite already exist?
9. What real-game GNM behavior is outside OpenGNM's coverage?
10. Are the relevant licenses compatible with a public test repository?

## Stage-0 success criterion

One PS4 ELF built from public source must:

- issue a real GNM raster draw using shaders compiled from source;
- synchronize completion;
- expose an unambiguous oracle;
- emit `SHADTEST ... status=PASS|FAIL`;
- be launchable by a small host runner.

A successful build alone is not sufficient. A successful EOP event alone is not sufficient.
The GPU must have produced data that is checked.

## Explicit non-goals for stage 0

- packaging a PKG;
- a comprehensive PS4 ABI/libkernel suite;
- reproducing undocumented commercial-game command streams;
- testing every shader stage;
- solving shadPS4's general headless frontend architecture;
- committing commercial integration tests.
