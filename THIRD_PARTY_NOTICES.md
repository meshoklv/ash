# Third-party notices

Nothing below is vendored in this repository; each item is fetched or installed by you.

| Component | Used for | License | How it is obtained |
|---|---|---|---|
| [pspdev / pspsdk](https://github.com/pspdev/pspdev) | PSP toolchain and SDK libraries linked into the EBOOTs | BSD-style / GPL-with-exceptions (see the project) | you install a release |
| [Lua 5.0.3](https://www.lua.org/ftp/lua-5.0.3.tar.gz) | enemy-AI virtual machine | MIT | `tools/ai/build_lua50.sh` downloads it from lua.org and checks its sha256; one typedef is patched at build time |
| [Soulstruct](https://github.com/Grimrukh/soulstruct), [soulstruct-havok](https://github.com/Grimrukh/soulstruct-havok) | reading game files in the cooking tools | see those repositories | `uv pip install` from git (not distributed here) |
| Pillow, NumPy, pyfqmr, colorama | cooking tools | HPND / BSD / MIT | `pip` |
| [PPSSPP](https://github.com/hrydgard/ppsspp) (headless) | optional emulator for the test scripts | GPL-2.0+ | built by you, never linked into Ash |
| DejaVu fonts | text rendering baked into the HUD data at cook time (system font on the build host) | Bitstream Vera / public domain changes | taken from your OS |
| EB Garamond (optional) | menu font baked at cook time | SIL OFL 1.1 | you download it from Google Fonts |

Game content (models, textures, animations, parameters, text, AI scripts) belongs to FromSoftware / Bandai Namco and is never part of this repository. The small icons in `dist-res/`, `src/save/` and `src/bench/` are original artwork.
