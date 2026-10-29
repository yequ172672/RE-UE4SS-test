# IterateGameDirectories

Returns a table of all game directories.

On Windows, when the executable is outside the usual project `Binaries/Win64` directory, UE4SS also accepts a `-basedir=` launch argument pointing to an existing project `Binaries/Win64` directory with a sibling `Content` directory. The project appears as `Game` in the returned table.

An example of an absolute path to `Win64`: `Q:\SteamLibrary\steamapps\common\Deep Rock Galactic\FSD\Binaries\Win64`.

To get to the same directory, do `IterateGameDirectories().<Game Name>.Binaries.Win64`.

- You can use `.__name` and `.__absolute_path` to retrieve values.
- You can use `.__files` to retrieve a table containing all files in this directory.
- You also use `.__name` and `.__absolute_path` for files.

## Return Value

| # | Type | Information |
|---|------|-------------|
| 1 | table | The game directories table |

## Example

```lua
for _, GameDirectory in pairs(IterateGameDirectories()) do
    print(GameDirectory.__name)
    print(GameDirectory.__absolute_path)
end
```
