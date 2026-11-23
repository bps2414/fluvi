# AGENTS.md

Guidance for anyone changing this repository, human or coding agent. Read it before writing code.

## Project

Fluvi is a native Windows client for Xbox Remote Play (xHome): C++20, Direct3D 12, no browser runtime. It receives
the console's WebRTC stream, decodes on the GPU, cleans and scales the image, optionally generates frames, and sends
gamepad input back. Windows 10/11 x64 only.

## Layout

| Path | What lives there |
|---|---|
| `include/fluvi/<module>/` | Public contracts of each module. The only headers other modules may include. |
| `src/<module>/` | Implementations. Never include another module's `src/` headers. |
| `shaders/` | HLSL compute shaders, compiled with DXC at build time. |
| `assets/ui/` | RmlUi screens (`.rml`) and styles (`.rcss`), loaded at run time. |
| `tests/<module>/` | doctest suites, one executable per module. |
| `cmake/`, `scripts/` | Build helpers. |
| `third_party/` | Only what is downloaded by `scripts/fetch-deps.ps1`; libraries come from vcpkg. |

Modules: `core`, `gpu` (core, present, pipeline, backends), `xhome` (`src/net/xhome`), `media`, `input`, `audio`,
`ui`, `app` (`fluvi.exe`).

## Build and test

Run from PowerShell (from bash, the `;` in `-DFLUVI_MODULES=a;b` is lost):

```powershell
scripts\dev.cmd pwsh scripts/fetch-deps.ps1    # FidelityFX SDK and FFmpeg, once
scripts\dev.cmd cmake --preset debug            # or release
scripts\dev.cmd cmake --build build\debug
scripts\dev.cmd ctest --preset debug
```

- `scripts\dev.cmd` finds Visual Studio with vswhere. Machine-specific paths go in `scripts\dev.local.cmd`
  (untracked): `FLUVI_VS`, `VCPKG_ROOT`, vcpkg cache variables, `FLUVI_VCPKG_INSTALL_OPTIONS`, `FLUVI_FFMPEG_DIR`,
  `FLUVI_DEPS_CACHE`.
- Build one module with `-DFLUVI_MODULES=gpu` (or `pipeline`, `xhome`, `media`, `input`, `audio`, `ui`, `app`).
- Tests that need a real GPU or device carry the ctest label `gpu` or `device`; skip them with `-LE "gpu|device"`.
- The build must stay warning-free (`/W4 /permissive-`).

## Architecture rules

- **Contracts are stable.** Changes to `include/fluvi/**` are additive. A breaking change needs a reason in the
  commit body and every caller updated in the same commit.
- **Extend, don't rewrite.** Code that exists and passes its tests is fixed or extended, not replaced by a new
  version. Search before adding a function, class, shader, RCSS class or design token; reuse what is there.
- **Errors:** no exceptions cross module boundaries. Return `fluvi::Status` (factories take `Status&`).
- **Logging:** `fluvi::log` only; no `printf` or `std::cout` in modules.
- **Latency first:** stages hand frames over through `LatestQueue`, which drops the old item instead of queueing.
  Never add a buffer that lets delay accumulate.
- **Testable without hardware:** pure logic (parsers, clocks, controllers) stays separate from device code so tests
  run with simulated inputs.
- **Dependencies** come from vcpkg (`vcpkg.json`). Vendored sources are allowed only when patched, as in
  `src/ui/rml_dx12/`, with the upstream license kept next to them.
- **Ported code** keeps an attribution header: project, license, upstream file, commit, and what changed.

## Code style

- C++20, MSVC. 4-space indent, 120 columns, braces on the same line (`.editorconfig`).
- Every source file starts with:
  ```cpp
  // SPDX-License-Identifier: GPL-3.0-only
  // Fluvi <module>: one line on what this file is.
  ```
- Headers use `#pragma once`. Code lives in `namespace fluvi::<module>`, closed with `} // namespace fluvi::<module>`.
- Naming: types, enum values and functions in `PascalCase` (`Device::CreateTexture`); locals, parameters and
  struct fields in `camelCase`; private members end in `_` (`device_`). The xhome port keeps Veyra's `camelCase`
  methods: follow the file you are in.
- Comments say why, not what. No references to plans, tickets, agents or people.
- Shaders: one pass per file, `PascalCase.hlsl`.

## UI

- `assets/ui/theme.rcss` is the design system (documented at the top of the file). Screens use only its tokens and
  shared classes: no loose colors, spacing or durations.
- Pending data shows a skeleton (`.skeleton*` classes), never a spinner or an empty screen.
- The brand is "Fluvi" (assets in `assets/ui/brand/`). Never use "Xbox" as a brand.

## Language

Code, comments, logs, `Status` messages, docs and commits are in English. User-facing UI text is currently pt-BR
and will move to translation files.

## Commits

- [Conventional Commits](https://www.conventionalcommits.org/): `type(scope): summary`.
  - Types: `feat`, `fix`, `perf`, `refactor`, `test`, `docs`, `build`, `chore`.
  - Scopes: module names (`core`, `gpu`, `pipeline`, `xhome`, `media`, `input`, `audio`, `ui`, `app`) or omitted.
- Summary in the imperative, lower case, no period, at most 72 characters.
- Every commit has a body, wrapped at 72 characters. Write it for a reader who does not program:
  - what was wrong or missing before the change;
  - what the change does, naming the files or parts involved;
  - what it means for the person who uses the app, with numbers when there are any.

  Explain each technical term the first time it appears. Use short paragraphs, or a list when the commit has
  several separate changes.
- Write the body by the [unslop](https://github.com/cursor/plugins/tree/main/pstack/skills/unslop) rules: plain
  words, active voice, one idea per sentence, numbers instead of vague words, no filler, no em dashes or dashes as
  punctuation, no arrows or unexplained abbreviations.
- One logical change per commit; the build and tests pass at every commit.
- Never commit build output, downloaded SDKs, secrets, tokens, personal notes or machine-specific paths.
- No tool-generated trailers (`Co-Authored-By`, "Generated with ...").
