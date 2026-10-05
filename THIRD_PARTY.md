# Third-party components

This repository does not vendor the dependencies below in git. The bootstrap script checks
them out under `.deps/` at revisions recorded in `deps.lock`.

| Component | Role | License / status |
|---|---|---|
| OpenOrbis/OpenOrbis-PS4-Toolchain | PS4 compiler/sysroot/link stubs/static libc | GPL-3.0 project; bundled components have their own licenses |
| PS4-OpenGNM/opengnm | clean/open GNM/GPA implementation | MIT |
| PS4-OpenGNM/opengnm-psbc | SPIR-V -> PS4 GCN shader compiler | MIT project, with vendored Mesa code under upstream licenses |
| PS4-OpenGNM/freegnm-examples | small shared support layer used by prototype | MIT |
| lateleite/freegnm, C branch | legacy source-only GNM/GNF/PSSL compatibility headers used by examples | MIT |
| KhronosGroup/SPIRV-Headers | psbc build dependency | MIT-style Khronos license |
| KhronosGroup/Vulkan-Headers | psbc build dependency | Apache-2.0 |

## Explicitly excluded

Do not add any of the following to this repository:

- Sony SDK headers/libraries obtained from an official SDK;
- extracted Sony PRX/sysmodules;
- commercial game executables, shaders, textures or other assets;
- keys, licenses, RIFs or decrypted game content.

The raw-ELF shadPS4 path is deliberately preferred because it does not require the
`sce_module/libc.prx` and `sce_module/libSceFios2.prx` files that OpenOrbis PKG examples
normally copy into installable homebrew packages.
