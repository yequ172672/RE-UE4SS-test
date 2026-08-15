# Custom Game Configs

> [!IMPORTANT]
> Some of these files may be out of date as the games/UE4SS updates. If you find that a game's custom game config is out of date, please open an issue on the UE4SS-RE/RE-UE4SS repository. Make sure that you first test if the game works without the custom game config, as it may have been fixed in the latest version of UE4SS.

These settings are for games that have altered the engine in ways that make UE4SS not work out of the box.  

Download the folder for your game into `ue4ss/CustomGameConfigs`. For example, the Kingdom Hearts 3 files use this structure:

```
Binaries/Win64/
├── ue4ss/
│   ├── CustomGameConfigs/
│   │   └── Kingdom Hearts 3/
│   │       ├── UE4SS_Signatures/
│   │       │   ├── FName_Constructor.lua
│   │       │   ├── FName_ToString.lua
│   │       │   └── StaticConstructObject.lua
│   │       ├── MemberVariableLayout.ini
│   │       ├── UE4SS-settings.ini
│   │       └── VTableLayout.ini
```

... but obviously the file structure will change depending on the game's configs.

If you download the zDEV version, all these files are already included in the zip file.

[You can find them here](https://github.com/UE4SS-RE/RE-UE4SS/tree/main/assets/CustomGameConfigs)

## Automatic selection

UE4SS searches these locations during startup:

1. `ue4ss/CustomGameConfigs/<config>/`
2. `<game executable directory>/CustomGameConfigs/<config>/`

Each configuration can include a `GameConfig.ini` selector:

```ini
[Game]
ExecutableName = ExampleGame-Win64-Shipping.exe
ProjectName = ExampleGame
InstallDirectoryName = Example Game
```

All non-empty selectors must match the running game. Matching ignores case, spaces, punctuation, and file-name separators. `ExecutableName` is the executable file name, `ProjectName` is the Unreal project directory above `Binaries`, and `InstallDirectoryName` is the outer installation directory. A selector match takes precedence over folder-name matching.

For older configuration packs without `GameConfig.ini`, UE4SS compares the normalized configuration folder name with those three runtime names. Keep the folder name game-specific to avoid ambiguous matches.

## Layered compatibility settings

A selected configuration can include `UE4SS-compatibility.ini`. UE4SS loads the installation's normal `UE4SS-settings.ini` first, then applies only the keys present in the compatibility file. This keeps local settings such as MCP, GUI, hotkeys, and mod paths intact while applying game-specific engine or hook overrides.

For compatibility with existing packs, a selected configuration's `UE4SS-settings.ini` is used as the overlay when `UE4SS-compatibility.ini` is absent.

The same selected directory supplies `MemberVariableLayout.ini`, `VTableLayout.ini`, and `UE4SS_Signatures/` overrides when present. Missing compatibility assets fall back to the installation defaults.