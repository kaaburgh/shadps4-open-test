# Third-party components

This repository does not vendor its stage-0 build dependencies in git. The bootstrap script
checks them out or installs them under `.deps/` at revisions recorded in `deps.lock`.

| Component | Role | License / status |
|---|---|---|
| OpenOrbis/OpenOrbis-PS4-Toolchain | PS4 compiler/sysroot/link/runtime inputs | GPL-3.0 project; bundled components have their own licenses |
| PS4-OpenGNM/opengnm (via the kaaburgh/opengnm fork pinned in `deps.lock`) | GNM/GPA implementation | MIT-labelled; `src/hwinit_sequences.h`, built only into its host backend, is copied from GPL-2.0-or-later shadPS4 code |
| PS4-OpenGNM/opengnm-psbc | SPIR-V -> PS4 GCN shader compiler; checked out at the pinned upstream revision and modified locally by `patches/opengnm-psbc/` | MIT project, with vendored Mesa code under upstream licenses; patched source files retain their upstream license headers |
| KhronosGroup/SPIRV-Headers | psbc host-build dependency | Khronos permissive/MIT-style license |
| KhronosGroup/Vulkan-Headers | psbc host-build dependency | Apache-2.0 |
| PS4-OpenGNM/freegnm-examples | research/reference examples only; not a stage-0 dependency | MIT |

## Explicitly excluded

Do not add any of the following to this repository:

- Sony SDK headers/libraries obtained from an official SDK;
- Sony PRX/sysmodules extracted from a console, SDK or commercial title;
- commercial game executables, shaders, textures or other assets;
- keys, licenses, RIFs or decrypted game content.

OpenOrbis PKG examples commonly contain `sce_module/libc.prx` and
`sce_module/libSceFios2.prx`. Current OpenOrbis source includes buildable stub/empty modules
with those names; they should not be confused with extracted proprietary Sony binaries.

The stage-0 shadPS4 workflow nevertheless uses a raw ELF because it removes package construction
and these package support modules entirely.

## Generated artifacts

The GLSL, SPIR-V and PS4 shader binaries produced from source authored in this repository do not
depend on commercial game material.

Before publishing a prebuilt guest ELF as a release artifact, perform one explicit audit of the
licenses/notices for every statically linked OpenOrbis runtime object. Until then, source plus a
reproducible build is the canonical distribution.
