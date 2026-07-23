# AGENTS.md

## Cursor Cloud specific instructions

### What this repository is
This is a **native Windows Mumble plugin** (compiled to a `.dll`) that adds
positional / proximity voice audio to the game *Conan Exiles*. It is a single
C project, **not** a monorepo, and has **no** web/server/service component,
no database, no Docker, and no test suite.

- Source: `plugin.c` / `plugin.h` (Win32 API C), vendored Mumble SDK headers
  (`MumbleAPI_v_1_0_x.h`, `MumblePlugin_v_1_0_x.h`, `PluginComponents_v_1_0_x.h`),
  `Resource.rc` + `resource.h`, and `.bmp`/`.ico` UI assets.
- Build definition: `Conan-Exiles-Mumble.vcxproj` (MSBuild / Visual Studio 2022,
  Platform Toolset **v143**, Windows 10 SDK, NuGet `Microsoft.Windows.CppWinRT`).

### Intended development environment (cannot run on this Linux VM)
The project is meant to be opened and built in **Visual Studio 2022 on Windows**
(MSVC v143 + Windows 10 SDK), producing `Conan-Exiles-Mumble.dll`. That DLL is
then loaded into the **Mumble client** (Settings → Plugins → Install) and tested
alongside a running **Conan Exiles** game (with its companion coordinate-writing
mod) and a **Mumble server**. None of these (MSVC/Windows SDK, the Windows Mumble
client, the game) exist or are automatable on this headless Linux Cloud VM, so the
plugin **cannot be built with its intended toolchain, nor run/end-to-end tested here.**

### What IS set up here (best-effort Linux toolchain)
`mingw-w64` (the `x86_64-w64-mingw32-gcc` cross-compiler) is installed. It is **not**
the project's official toolchain, but it resolves all the Win32 headers the code uses
and lets you compile / syntax-check the C source and compile the resource script:

```
# Syntax-check / compile the plugin translation unit
x86_64-w64-mingw32-gcc -c plugin.c -o /tmp/plugin.o -municode \
  -DUNICODE -D_UNICODE -DWIN32 -DNDEBUG -DCONANEXILESMUMBLE_EXPORTS -D_WINDOWS -D_USRDLL

# Compile the Win32 resource script (Resource.rc is UTF-16; the null-char warnings are benign)
x86_64-w64-mingw32-windres Resource.rc -O coff -o /tmp/resource.res
```

There is **no lint config, no automated tests, and no build/run script** in this repo,
so there is nothing of that kind to run.

### Pre-existing source issues (do NOT assume the tree compiles cleanly)
As committed, `plugin.c` does **not** compile — and these are compiler-agnostic
source-level problems that would also fail under MSVC, so they are unrelated to the
Linux/MinGW setup:
- `plugin.c` references `VoiceRangePreset` members `whisperKey`, `normalKey`,
  `shoutKey`, `voiceToggleKey`, but the `VoiceRangePreset` struct in `plugin.h`
  does not declare them (20 errors).
- `writeFullConfiguration`, `saveDefaultSettingsToConfig`, and
  `loadDefaultSettingsFromConfig` have `static` definitions that follow non-`static`
  declarations (3 errors).

Also note `Conan-Exiles-Mumble.vcxproj` still contains Visual Studio **template
tokens** (`{$guid1$}`, `$safeprojectname$`) and a `.vstemplate` file is present, so
the committed project is a VS *template* rather than a directly-buildable project.

Do not treat the above as environment breakage; fix them only if a task explicitly
asks you to change the plugin source/project.
