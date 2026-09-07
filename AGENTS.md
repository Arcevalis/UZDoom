# UZDoom — Agent Guide

Modern DOOM source port (ZDoom/GZDoom continuation). OpenGL/Vulkan renderer, ZScript/DECORATE scripting, extensive modding support. Licensed GPL-3.0-or-later.

## Build
- **C++20 strict** (`CMAKE_CXX_EXTENSIONS OFF`)
- CMake ≥ 3.16, Ninja preferred:
  ```sh
  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build build
  ```
- Default build type is **Debug** if unspecified.
- **Fast math** (`-ffast-math`) is NOT global — applied per-file only to rendering/texture/Vulkan sources.
- Floating-point contract is `-ffp-contract=off` globally (GCC/Clang) for reproducible float behavior.

## Key CMake options
| Option | Default | Note |
|--------|---------|------|
| `HAVE_VULKAN` | ON | Vulkan renderer |
| `HAVE_VM_JIT` | ON (x86_64) | VM JIT via asmjit |
| `BUILD_NONFREE` | ON | Non-free assets (brightmaps, widescreen gfx) |
| `NO_OPENAL` | OFF | Disable OpenAL sound |
| `DYN_OPENAL` | ON | Load OpenAL dynamically |
| `DYN_GTK` | ON | Load GTK at runtime (Linux) |
| `PK3_QUIET_ZIPDIR` | OFF | Silence PK3 builder output |
| `WITH_ASAN` / `WITH_UBSAN` | 0 | Sanitizers (GCC/Clang only) |

## Quality
- **No test framework** — quality enforced by compilation + CI. `ctest` / GTest not used.
- **No linter** — formatting checked via `.clang-format` (Microsoft style, `UseTab: AlignWithSpaces`) and `tools/format-spaces.sh`.
- **IWYU** enabled by default (`ENABLE_IWYU=ON`), mapping in `tools/iwyu.imp`.

## Generated code (built during compilation, not committed)
- `xlat_parser.c/h` — from `gamedata/xlat/xlat_parser.y` via `lemon`
- `zcc-parse.c/h` — from `scripting/frontend/zcc-parse.lemon` via `lemon`
- `sc_man_scanner.h` — from `common/engine/sc_man_scanner.re` via `re2c`
- `gitinfo.h` — from `cmake/gitinfo.h.in` via `UpdateRevision.cmake`

## Conventions
- **AI-generated art/code banned** (CONTRIBUTING.md). Offense = org-wide ban.
- New source files must carry SPDX header: `SPDX-License-Identifier: GPL-3.0-or-later`
- All files: UTF-8, LF endings, tab indentation, trailing whitespace trimmed, final newline.
- String translation via Weblate — do **not** edit `language.*` files directly.
- **Git subtrees** from `UZDoom/` org: `ZWidget`, `ZMusic`, `Translation`, `ZVulkan`. Update via `tools/update-subtrees.sh`.

## CI quirks
- Linux CI uses `debian:oldstable` container to link against the oldest possible glibc for maximum binary compatibility.
- macOS CI is x86_64 (Rosetta for ARM). No native Apple Silicon build yet.
- Release builds: MSVC static CRT (`/MT`), LTCG enabled, stripped.

## Version
- `VERSIONSTR "5.0.0-pre"`, `GAMESIG "UZDOOM"` (also loads saves from `"LZDOOM"`).
- `GetVersionString()` prefers `GIT_DESCRIPTION` from `gitinfo.h` over static string.
