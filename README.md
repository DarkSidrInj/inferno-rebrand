# bycmd — reorganized source tree

The source folders now follow the domain-based layout used by `fimozrm`, while preserving bycmd's existing implementation and Android `ndk-build` pipeline. This is a structural reorganization, not a rewrite of the feature logic.

## Layout

- `src/CConfigs/` — menu/config declarations
- `src/CGame/` — game state and player data
- `src/CMath/` — vector and game math helpers
- `src/CMenu/` — menu, widgets, theme
- `src/CProtection/` — obfuscation/protection helpers
- `src/CUtils/` — process memory access and diagnostics
- `src/CVisual/` — ESP / visual manager
- `includes/` — ImGui, Android drawing/touch backends, and fonts
- `jni/` — Android NDK build manifests

## Build

Run `sh compile.sh` from the project directory after ensuring the Android NDK is available at `../../android-ndk-r30` or set `ANDROID_NDK_HOME`. The generated executable is copied to `out/arm64-v8a/bycmd-1.0.0`.

## GameMgr entity discovery

`src/CGame/CGameContext.hpp` now tries the external mirror of the internal
`PlayerLoopHelper -> Runners -> TickableLoopItem -> GameMgr -> World -> Registry
-> Store -> Entities` walk first. It reads target memory through the existing
`mem_read()` / `rpm<T>()` API in `src/CUtils/CMemory.hpp`; it does not call
IL2CPP functions inside the game process or dereference target addresses as
local pointers.

The `PlayerLoopHelper_TypeInfo` RVA is `0xC1FCEC8`. The code validates each
candidate manager by checking the exact Registry and Store class pointers, and
uses the existing Registry memory scan as a fallback. Useful log lines are
`PlayerLoopHelper resolved`, `GameMgr chain found`, and `entity source=GameMgr`.
The class static-fields offset is probed at the supplied profile value (`0x40`)
then the common IL2CPP layout candidate (`0xB8`); the correct candidate still
needs confirmation from runtime logs on the target game build.

## Camera and ESP (skeed port, x64)

Ported from the ARM32 `skeed` source. Every ARM32 offset was re-derived for ARM64 from
`dump.cs` (see `src/CGame/COffsets.hpp`; the skeed value is in each comment).

| module | what it does |
| --- | --- |
| `CGame/CCamera.hpp` | GameCamera holder (`xmt : nrl`, Camera at `+0x20`) -> managed Camera -> native Camera -> live view/projection |
| `CGame/CClass.hpp` | class names (`cim`, `qak`, `njg`, `ake`); the name offset in Il2CppClass is measured, not assumed |
| `CGame/CWorld.hpp` | grenades (`cim`, `qak`) and dropped weapons (`njg`) from the entity list |
| `CGame/CSkeleton.hpp` | bones via jgi -> CharacterBindings -> BonesDictionary, one hierarchy read per player |
| `CVisual/CWorldESP.cpp` | skeleton, grenade and dropped weapon drawing |

The camera is the game's own, so shot animation and the scoped FOV are exact. The old
candidate scoring and the synthetic camera (`Fallback FOV`) remain only as fallback.

Log lines to check after a build:

```
Il2CppClass name offset = +0x..        class names resolved
game camera found holder=0x..          camera holder located
CAMERA source=game camera holder fov=  live FOV, changes when scoping
BonesDictionary inner Dictionary found at +0x..
WORLD entities=.. grenades=.. drops=..
SKELETON enemies=.. cached=.. live=..
```

`tests/selftest_port.cpp` runs the new logic against fake in-process memory (host build):

```
g++ -std=c++17 -Iincludes/internal/ImGui -Iincludes/internal -Iincludes -Isrc tests/selftest_port.cpp -o selftest && ./selftest
```

`tests/menu_preview.cpp` renders the real menu (`src/CMenu/CMenu.cpp`) on the host with Dear ImGui into a software
framebuffer, to look at it without a phone (pages 0..4: Players, World, Skins, Miscellaneous, Settings; the optional
second argument opens the colour picker, a select popup or a scrolled page):

```
g++ -std=c++17 -O1 -w -Itests/stubs -Iincludes -Iincludes/fonts -Iincludes/internal -Iincludes/internal/ImGui -Iincludes/draw/ImGui -Isrc \
    tests/menu_preview.cpp includes/draw/ImGui/imgui.cpp includes/draw/ImGui/imgui_draw.cpp includes/draw/ImGui/imgui_tables.cpp \
    includes/draw/ImGui/imgui_widgets.cpp -o menu_preview && ./menu_preview 0 "" out.ppm
```
