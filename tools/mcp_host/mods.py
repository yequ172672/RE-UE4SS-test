from __future__ import annotations

import json
from pathlib import Path
import re
import shutil
import tempfile
from typing import Any

from .model import ToolError
from .workspace import Workspace, remove_tree, sha256_file


MOD_NAME_PATTERN = re.compile(r"[A-Za-z_][A-Za-z0-9_]{0,79}\Z")
MAX_MOD_FILES = 1000



class ModManager:
    def __init__(self, workspace: Workspace) -> None:
        self.workspace = workspace

    @staticmethod
    def validate_name(name: str) -> None:
        if not MOD_NAME_PATTERN.fullmatch(name):
            raise ToolError(
                "mod.invalid_name",
                "Mod name must be an identifier containing at most 80 ASCII letters, digits, or underscores",
                stage="mod.validate",
                context={"name": name},
            )

    def create_lua(
        self,
        root: str,
        directory: str,
        name: str,
        *,
        entry_script: str | None = None,
        metadata: dict[str, Any] | None = None,
        overwrite: bool = False,
    ) -> dict[str, Any]:
        self.validate_name(name)
        base = self.workspace.resolve(root, str(Path(directory) / name))
        if base.path.exists() and any(base.path.iterdir()) and not overwrite:
            raise ToolError("mod.conflict", "The destination mod directory is not empty", stage="mod.create", context={"path": base.relative})
        base.path.mkdir(parents=True, exist_ok=True)
        scripts = base.path / "Scripts"
        scripts.mkdir(parents=True, exist_ok=True)
        main = scripts / "main.lua"
        source = entry_script if entry_script is not None else f'print("[{name}] loaded")\n'
        main.write_text(source, encoding="utf-8", newline="\n")
        files = [main]
        if metadata:
            metadata_path = base.path / "mod.json"
            metadata_path.write_text(json.dumps({"name": name, **metadata}, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
            files.append(metadata_path)
        artifacts = [
            {"path": file.relative_to(base.root).as_posix(), "size": file.stat().st_size, "sha256": sha256_file(file)} for file in files
        ]
        return {
            "currentState": {"type": "lua", "name": name, "root": root, "path": base.relative, "artifacts": artifacts},
            "actualChanges": [{"kind": "mod_source", "action": "created", "path": base.relative}],
            "evidence": [{"kind": "artifact_manifest", "source": base.path.as_posix(), "artifacts": artifacts}],
        }

    def create_cpp(
        self,
        root: str,
        directory: str,
        name: str,
        *,
        ue4ss_source: str,
        description: str = "UE4SS C++ mod",
        author: str = "UE4SS Mod Developer",
        overwrite: bool = False,
    ) -> dict[str, Any]:
        self.validate_name(name)
        ue4ss_path = Path(ue4ss_source).expanduser().resolve()
        if not (ue4ss_path / "CMakeLists.txt").is_file():
            raise ToolError(
                "mod.ue4ss_source_invalid",
                "ue4ssSource must identify a RE-UE4SS source directory containing CMakeLists.txt",
                stage="mod.create",
                context={"ue4ssSource": ue4ss_path.as_posix()},
            )
        cmake_ue4ss_source = ue4ss_path.as_posix().replace('"', '\\"')
        base = self.workspace.resolve(root, str(Path(directory) / name))
        if base.path.exists() and any(base.path.iterdir()) and not overwrite:
            raise ToolError("mod.conflict", "The destination mod directory is not empty", stage="mod.create", context={"path": base.relative})
        base.path.mkdir(parents=True, exist_ok=True)
        cmake = base.path / "CMakeLists.txt"
        source = base.path / "dllmain.cpp"
        cmake.write_text(
            "\n".join(
                [
                    "cmake_minimum_required(VERSION 3.22)",
                    f"project({name} LANGUAGES CXX)",
                    f'add_subdirectory("{cmake_ue4ss_source}" RE-UE4SS EXCLUDE_FROM_ALL)',
                    f"add_library({name} SHARED dllmain.cpp)",
                    f"target_link_libraries({name} PRIVATE UE4SS)",
                    f"set_target_properties({name} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON OUTPUT_NAME main)",
                    f"install(TARGETS {name} RUNTIME DESTINATION dlls)",
                    "",
                ]
            ),
            encoding="utf-8",
            newline="\n",
        )
        macro = re.sub(r"[^A-Za-z0-9]", "_", name).upper() + "_API"
        safe_description = description.replace("\\", "\\\\").replace('"', '\\"').replace("\r", " ").replace("\n", " ")
        safe_author = author.replace("\\", "\\\\").replace('"', '\\"').replace("\r", " ").replace("\n", " ")
        source.write_text(
            f'''#include <Mod/CppUserModBase.hpp>\n\nclass {name} final : public RC::CppUserModBase\n{{\n  public:\n    {name}()\n    {{\n        ModName = STR("{name}");\n        ModVersion = STR("1.0.0");\n        ModDescription = STR("{safe_description}");\n        ModAuthors = STR("{safe_author}");\n    }}\n}};\n\n#define {macro} __declspec(dllexport)\nextern "C"\n{{\n    {macro} RC::CppUserModBase* start_mod()\n    {{\n        return new {name}();\n    }}\n\n    {macro} void uninstall_mod(RC::CppUserModBase* mod)\n    {{\n        delete mod;\n    }}\n}}\n''',
            encoding="utf-8",
            newline="\n",
        )
        artifacts = [
            {"path": file.relative_to(base.root).as_posix(), "size": file.stat().st_size, "sha256": sha256_file(file)} for file in (cmake, source)
        ]
        return {
            "currentState": {"type": "cpp", "name": name, "root": root, "path": base.relative, "artifacts": artifacts},
            "actualChanges": [{"kind": "mod_source", "action": "created", "path": base.relative}],
            "evidence": [{"kind": "artifact_manifest", "source": base.path.as_posix(), "artifacts": artifacts}],
        }

    def install(
        self,
        source_root: str,
        source_path: str,
        destination_root: str,
        mods_path: str,
        name: str,
        *,
        overwrite: bool = False,
    ) -> dict[str, Any]:
        self.validate_name(name)
        source = self.workspace.resolve(source_root, source_path, must_exist=True)
        destination = self.workspace.resolve(destination_root, str(Path(mods_path) / name))
        if not source.path.is_dir():
            raise ToolError("mod.source_not_directory", "Mod source must be a directory", stage="mod.install")
        if source.path == destination.path:
            raise ToolError("mod.source_is_destination", "Mod source and install destination must differ", stage="mod.install")
        source_manifest = self._manifest(source.path)
        if not source_manifest:
            raise ToolError("mod.source_empty", "Mod source does not contain any files", stage="mod.install")
        destination_existed = destination.path.exists()
        conflicts: list[dict[str, Any]] = []
        if destination_existed:
            destination_manifest = self._manifest(destination.path)
            for relative in sorted(set(source_manifest) | set(destination_manifest)):
                source_item = source_manifest.get(relative)
                installed_item = destination_manifest.get(relative)
                source_hash = source_item.get("sha256") if source_item else None
                installed_hash = installed_item.get("sha256") if installed_item else None
                if source_hash != installed_hash:
                    conflicts.append({"path": relative, "sourceSha256": source_hash, "installedSha256": installed_hash})
            if not conflicts:
                artifacts = [{"path": relative, **item} for relative, item in sorted(destination_manifest.items())]
                return {
                    "currentState": {"name": name, "installed": True, "path": destination.relative, "artifacts": artifacts},
                    "actualChanges": [],
                    "evidence": [{"kind": "artifact_manifest", "source": destination.path.as_posix(), "artifacts": artifacts}],
                    "restartRequired": False,
                }
            if not overwrite:
                raise ToolError(
                    "mod.install_conflict",
                    "Installed mod contents differ from the source",
                    stage="mod.install",
                    context={"name": name, "conflicts": conflicts},
                    suggested_actions=["Back up the installed mod, then retry with overwrite=true"],
                )
        destination.path.parent.mkdir(parents=True, exist_ok=True)
        temporary = Path(tempfile.mkdtemp(prefix=f".{name}-", dir=destination.path.parent))
        staged = temporary / "staged"
        backup = temporary / "backup"
        backup_created = False
        try:
            shutil.copytree(source.path, staged)
            if destination.path.exists():
                try:
                    destination.path.replace(backup)
                    backup_created = True
                except OSError as error:
                    raise ToolError(
                        "mod.install_commit_failed",
                        "Could not move the existing mod into the rollback backup",
                        stage="mod.commit",
                        context={"path": destination.relative, "phase": "backup_existing", "error": str(error)},
                        retryable=True,
                    ) from error
            try:
                staged.replace(destination.path)
            except OSError as error:
                rollback_error: OSError | None = None
                if backup_created:
                    try:
                        backup.replace(destination.path)
                        backup_created = False
                    except OSError as restore_error:
                        rollback_error = restore_error
                context = {"path": destination.relative, "phase": "commit_staged", "error": str(error)}
                if rollback_error is not None:
                    context.update({"rollbackError": str(rollback_error), "backupPath": backup.as_posix()})
                raise ToolError(
                    "mod.install_commit_failed",
                    "Could not commit the staged mod; the prior installation was restored"
                    if rollback_error is None
                    else "Could not commit the staged mod or restore the prior installation",
                    stage="mod.commit",
                    context=context,
                    retryable=rollback_error is None,
                ) from error

            installed_manifest = self._manifest(destination.path)
            drift = [
                relative
                for relative, item in source_manifest.items()
                if installed_manifest.get(relative, {}).get("sha256") != item["sha256"]
            ]
            if drift:
                failed_install = temporary / "failed-install"
                try:
                    destination.path.replace(failed_install)
                    if backup_created:
                        backup.replace(destination.path)
                        backup_created = False
                    remove_tree(failed_install)
                except OSError as rollback_error:
                    raise ToolError(
                        "mod.install_rollback_failed",
                        "Installed files failed verification and the prior installation could not be restored",
                        stage="mod.verify",
                        context={"drift": drift, "rollbackError": str(rollback_error), "backupPath": backup.as_posix()},
                    ) from rollback_error
                raise ToolError(
                    "mod.install_verification_failed",
                    "Installed files did not match source hashes; the prior installation was restored",
                    stage="mod.verify",
                    context={"drift": drift},
                )

            artifacts = [{"path": relative, **item} for relative, item in sorted(installed_manifest.items())]
            result: dict[str, Any] = {
                "currentState": {"name": name, "installed": True, "path": destination.relative, "artifacts": artifacts},
                "actualChanges": [{"kind": "mod", "action": "installed", "path": destination.relative, "overwritten": destination_existed}],
                "evidence": [{"kind": "artifact_manifest", "source": destination.path.as_posix(), "artifacts": artifacts}],
                "restartRequired": True,
            }
            if backup_created:
                try:
                    remove_tree(backup)
                    backup_created = False
                except OSError as error:
                    cleanup_error = ToolError(
                        "mod.install_backup_cleanup_failed",
                        "The new mod was verified, but the rollback backup could not be removed",
                        stage="mod.cleanup",
                        context={"path": destination.relative, "backupPath": backup.as_posix(), "error": str(error)},
                        retryable=True,
                    )
                    result.update(
                        {
                            "status": "partial",
                            "currentState": {
                                **result["currentState"],
                                "cleanupPending": True,
                                "backupPath": backup.as_posix(),
                                "cleanupError": str(error),
                            },
                            "errors": [cleanup_error.as_dict()],
                            "suggestedActions": ["Remove the reported rollback backup after releasing any process that has it open"],
                        }
                    )
            return result
        finally:
            if staged.exists():
                try:
                    remove_tree(staged)
                except OSError:
                    pass
            if temporary.exists() and not backup.exists():
                try:
                    temporary.rmdir()
                except OSError:
                    pass

    def set_enabled(self, root: str, mods_path: str, name: str, enabled: bool) -> dict[str, Any]:
        self.validate_name(name)
        mods = self.workspace.resolve(root, mods_path, must_exist=True)
        text_manifest = mods.path / "mods.txt"
        previous: bool | None = None

        if text_manifest.is_file():
            try:
                lines = text_manifest.read_text(encoding="utf-8-sig").splitlines()
            except OSError as error:
                raise ToolError("mod.manifest_invalid", str(error), stage="mod.enable", context={"path": text_manifest.as_posix()}) from error
            entry_pattern = re.compile(r"^\s*([^;][^:]*?)\s*:\s*([01])\s*$")
            found_index: int | None = None
            for index, line in enumerate(lines):
                match = entry_pattern.match(line)
                if match and match.group(1).strip() == name:
                    found_index = index
                    previous = match.group(2) == "1"
                    break
            replacement = f"{name} : {1 if enabled else 0}"
            if found_index is not None:
                lines[found_index] = replacement
            else:
                insert_at = next((index for index, line in enumerate(lines) if line.lstrip().startswith("; Built-in keybinds")), len(lines))
                lines.insert(insert_at, replacement)
            temporary = text_manifest.with_suffix(".txt.tmp")
            temporary.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
            temporary.replace(text_manifest)
            manifest_path = text_manifest
        else:
            manifest_path = mods.path / "mods.json"
            entries: list[dict[str, Any]] = []
            if manifest_path.is_file():
                try:
                    entries = json.loads(manifest_path.read_text(encoding="utf-8"))
                except (json.JSONDecodeError, OSError) as error:
                    raise ToolError("mod.manifest_invalid", str(error), stage="mod.enable", context={"path": manifest_path.as_posix()}) from error
            if not isinstance(entries, list) or not all(isinstance(entry, dict) for entry in entries):
                raise ToolError("mod.manifest_invalid", "mods.json must contain an array of objects", stage="mod.enable", context={"path": manifest_path.as_posix()})
            for entry in entries:
                if entry.get("mod_name") == name:
                    previous = bool(entry.get("mod_enabled"))
                    entry["mod_enabled"] = enabled
                    break
            else:
                entries.append({"mod_name": name, "mod_enabled": enabled})
            temporary = manifest_path.with_suffix(".json.tmp")
            temporary.write_text(json.dumps(entries, indent=4) + "\n", encoding="utf-8", newline="\n")
            temporary.replace(manifest_path)

        changes = []
        if previous != enabled:
            changes.append({"kind": "mod_state", "action": "enabled" if enabled else "disabled", "name": name, "previous": previous})
        return {
            "currentState": {"name": name, "enabled": enabled, "manifest": manifest_path.relative_to(mods.root).as_posix()},
            "actualChanges": changes,
            "evidence": [{"kind": "artifact_hash", "source": manifest_path.as_posix(), "sha256": sha256_file(manifest_path)}],
            "restartRequired": True,
        }

    def remove(self, root: str, mods_path: str, name: str, *, expected_manifest: list[dict[str, Any]] | None = None) -> dict[str, Any]:
        self.validate_name(name)
        destination = self.workspace.resolve(root, str(Path(mods_path) / name), must_exist=True)
        if not destination.path.is_dir():
            raise ToolError("mod.not_directory", "Installed mod path is not a directory", stage="mod.remove")
        actual = self._manifest(destination.path)
        if expected_manifest is not None:
            if not all(isinstance(entry, dict) and isinstance(entry.get("path"), str) and isinstance(entry.get("sha256"), str) for entry in expected_manifest):
                raise ToolError("mod.expected_manifest_invalid", "Expected manifest entries require path and sha256 strings", stage="mod.remove")
            expected = {entry["path"]: entry["sha256"] for entry in expected_manifest}
            drift = [
                relative
                for relative in sorted(set(actual) | set(expected))
                if actual.get(relative, {}).get("sha256") != expected.get(relative)
            ]
            if drift:
                raise ToolError(
                    "mod.remove_conflict",
                    "Installed mod changed after the expected manifest was captured",
                    stage="mod.remove",
                    context={"drift": drift},
                    suggested_actions=["Read the installed manifest and decide whether to back up or force a new removal"],
                )
        artifacts = [{"path": relative, **item} for relative, item in sorted(actual.items())]
        remove_tree(destination.path)
        return {
            "currentState": {"name": name, "installed": False, "path": destination.relative},
            "actualChanges": [{"kind": "mod", "action": "removed", "path": destination.relative, "artifacts": artifacts}],
            "restartRequired": True,
        }

    @staticmethod
    def _manifest(root: Path) -> dict[str, dict[str, Any]]:
        files: list[Path] = []
        for path in root.rglob("*"):
            if path.is_symlink():
                raise ToolError(
                    "mod.symlink_unsupported",
                    "Mod directories may not contain symbolic links",
                    stage="mod.manifest",
                    context={"root": root.as_posix(), "symlinks": [path.relative_to(root).as_posix()]},
                )
            if path.is_file():
                files.append(path)
                if len(files) > MAX_MOD_FILES:
                    raise ToolError(
                        "mod.too_many_files",
                        f"Mod directories may contain at most {MAX_MOD_FILES} files",
                        stage="mod.manifest",
                        context={"root": root.as_posix(), "limit": MAX_MOD_FILES},
                    )
        return {
            file.relative_to(root).as_posix(): {"size": file.stat().st_size, "sha256": sha256_file(file)}
            for file in sorted(files)
        }
