from __future__ import annotations

import base64
import binascii
from dataclasses import dataclass
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import tempfile
import threading
from typing import Any, Iterable
import zipfile

from .model import ToolError, utc_now


MAX_READ_BYTES = 4 * 1024 * 1024
MAX_SEARCH_MATCHES = 5000


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

def remove_tree(path: Path) -> None:
    def remove_readonly(function: Any, failing_path: str, _error: Any) -> None:
        os.chmod(failing_path, stat.S_IWRITE)
        function(failing_path)

    shutil.rmtree(path, onerror=remove_readonly)



@dataclass(frozen=True, slots=True)
class ScopedPath:
    root_name: str
    root: Path
    path: Path
    relative: str


class Workspace:
    def __init__(self, roots: dict[str, str | Path], state_directory: str | Path) -> None:
        if not roots:
            raise ValueError("At least one workspace root is required")
        self.roots = {name: Path(path).expanduser().resolve() for name, path in roots.items()}
        self.state_directory = Path(state_directory).expanduser().resolve()
        self.backup_directory = self.state_directory / "backups"
        self.backup_directory.mkdir(parents=True, exist_ok=True)
        self._mutation_locks = tuple(threading.RLock() for _ in range(256))

    def root_snapshot(self) -> list[dict[str, Any]]:
        return [
            {"name": name, "path": path.as_posix(), "available": path.is_dir()}
            for name, path in sorted(self.roots.items())
        ]

    def resolve(self, root_name: str, relative: str = "", *, must_exist: bool = False) -> ScopedPath:
        root = self.roots.get(root_name)
        if root is None:
            raise ToolError(
                "workspace.root_not_found",
                f"Workspace root '{root_name}' is not configured",
                stage="workspace.resolve",
                context={"root": root_name, "availableRoots": sorted(self.roots)},
            )
        candidate = (root / Path(relative)).resolve(strict=False)
        try:
            relative_path = candidate.relative_to(root)
        except ValueError as error:
            raise ToolError(
                "workspace.path_outside_root",
                "Path resolves outside the configured workspace root",
                stage="workspace.resolve",
                context={"root": root_name, "path": relative, "resolved": candidate.as_posix()},
                status="rejected",
            ) from error
        if must_exist and not candidate.exists():
            raise ToolError(
                "artifact.not_found",
                f"Path '{relative}' does not exist in root '{root_name}'",
                stage="workspace.resolve",
                context={"root": root_name, "path": relative},
            )
        return ScopedPath(root_name, root, candidate, relative_path.as_posix())

    def list(self, root: str, path: str = "", cursor: int = 0, limit: int = 200, recursive: bool = False) -> dict[str, Any]:
        scoped = self.resolve(root, path, must_exist=True)
        if not scoped.path.is_dir():
            raise ToolError("artifact.not_directory", "The requested path is not a directory", stage="files.list", context={"path": scoped.relative})
        limit = min(max(limit, 1), 1000)
        iterator: Iterable[Path] = scoped.path.rglob("*") if recursive else scoped.path.iterdir()
        entries = [
            item
            for item in iterator
            if self._is_within_root(item, scoped.root)
        ]
        entries.sort(key=lambda item: item.relative_to(scoped.path).as_posix().casefold())
        page = entries[max(cursor, 0) : max(cursor, 0) + limit]
        values = []
        for entry in page:
            stat = entry.stat()
            values.append(
                {
                    "path": entry.relative_to(scoped.root).as_posix(),
                    "type": "directory" if entry.is_dir() else "file",
                    "size": stat.st_size if entry.is_file() else None,
                    "modifiedNs": stat.st_mtime_ns,
                }
            )
        next_cursor = max(cursor, 0) + len(page)
        return {"root": root, "path": scoped.relative, "entries": values, "nextCursor": next_cursor, "endOfList": next_cursor >= len(entries)}

    def read(self, root: str, path: str, offset: int = 0, max_bytes: int = 65536, encoding: str = "utf-8") -> dict[str, Any]:
        scoped = self.resolve(root, path, must_exist=True)
        if not scoped.path.is_file():
            raise ToolError("artifact.not_file", "The requested path is not a file", stage="files.read", context={"path": scoped.relative})
        max_bytes = min(max(max_bytes, 1), MAX_READ_BYTES)
        size = scoped.path.stat().st_size
        offset = min(max(offset, 0), size)
        with scoped.path.open("rb") as stream:
            stream.seek(offset)
            content = stream.read(max_bytes)
        try:
            text = content.decode(encoding)
            payload: dict[str, Any] = {"content": text, "encoding": encoding, "binary": False}
        except (UnicodeDecodeError, LookupError):
            payload = {"content": base64.b64encode(content).decode("ascii"), "encoding": "base64", "binary": True}
        return {
            "root": root,
            "path": scoped.relative,
            "offset": offset,
            "returnedBytes": len(content),
            "nextOffset": offset + len(content),
            "endOfFile": offset + len(content) >= size,
            "size": size,
            **payload,
        }

    def write(
        self,
        root: str,
        path: str,
        content: str,
        *,
        encoding: str = "utf-8",
        expected_sha256: str | None = None,
        create_parents: bool = True,
    ) -> dict[str, Any]:
        scoped = self.resolve(root, path)
        if encoding == "base64":
            try:
                data = base64.b64decode(content, validate=True)
            except (binascii.Error, ValueError) as error:
                raise ToolError("artifact.invalid_base64", str(error), stage="files.write") from error
        else:
            try:
                data = content.encode(encoding)
            except LookupError as error:
                raise ToolError("artifact.invalid_encoding", str(error), stage="files.write") from error

        with self._mutation_lock(scoped):
            before_exists = scoped.path.exists()
            before_hash = sha256_file(scoped.path) if scoped.path.is_file() else None
            if expected_sha256 is not None and before_hash != expected_sha256:
                raise ToolError(
                    "artifact.conflict",
                    "The file changed after it was read",
                    stage="files.write",
                    context={"path": scoped.relative, "expectedSha256": expected_sha256, "actualSha256": before_hash},
                    retryable=True,
                    suggested_actions=["Read the current file and reapply the intended change"],
                )
            if scoped.path.exists() and not scoped.path.is_file():
                raise ToolError("artifact.not_file", "The destination exists and is not a file", stage="files.write", context={"path": scoped.relative})
            if create_parents:
                scoped.path.parent.mkdir(parents=True, exist_ok=True)
            elif not scoped.path.parent.is_dir():
                raise ToolError("artifact.parent_missing", "The destination parent does not exist", stage="files.write", context={"path": scoped.relative})

            with tempfile.NamedTemporaryFile(dir=scoped.path.parent, delete=False) as temporary:
                temporary.write(data)
                temporary_path = Path(temporary.name)
            try:
                os.replace(temporary_path, scoped.path)
            finally:
                temporary_path.unlink(missing_ok=True)
            after_hash = sha256_file(scoped.path)
            action = "overwritten" if before_exists else "created"
            return {
                "currentState": {"root": root, "path": scoped.relative, "size": len(data), "sha256": after_hash},
                "actualChanges": [{"kind": "file", "action": action, "path": scoped.relative, "beforeSha256": before_hash, "afterSha256": after_hash}],
                "evidence": [{"kind": "artifact_hash", "source": scoped.path.as_posix(), "sha256": after_hash, "size": len(data)}],
            }

    def move(self, root: str, source: str, destination: str, *, overwrite: bool = False) -> dict[str, Any]:
        source_path = self.resolve(root, source, must_exist=True)
        destination_path = self.resolve(root, destination)
        if destination_path.path.exists() and not overwrite:
            raise ToolError("artifact.conflict", "The destination already exists", stage="files.move", context={"path": destination_path.relative})
        destination_path.path.parent.mkdir(parents=True, exist_ok=True)
        if destination_path.path.exists():
            if destination_path.path.is_dir():
                remove_tree(destination_path.path)
            else:
                destination_path.path.unlink()
        shutil.move(source_path.path, destination_path.path)
        return {
            "currentState": {"root": root, "path": destination_path.relative, "exists": destination_path.path.exists()},
            "actualChanges": [{"kind": "path", "action": "moved", "from": source_path.relative, "to": destination_path.relative}],
        }

    def delete(self, root: str, path: str, *, recursive: bool = False, expected_sha256: str | None = None) -> dict[str, Any]:
        scoped = self.resolve(root, path)
        with self._mutation_lock(scoped):
            before_hash = sha256_file(scoped.path) if scoped.path.is_file() else None
            if expected_sha256 is not None and before_hash != expected_sha256:
                raise ToolError(
                    "artifact.conflict",
                    "The path changed after it was read",
                    stage="files.delete",
                    context={"path": scoped.relative, "expectedSha256": expected_sha256, "actualSha256": before_hash},
                )
            if not scoped.path.exists():
                raise ToolError(
                    "artifact.not_found",
                    f"Path '{path}' does not exist in root '{root}'",
                    stage="workspace.resolve",
                    context={"root": root, "path": path},
                )
            if scoped.path.is_dir():
                if not recursive:
                    scoped.path.rmdir()
                else:
                    remove_tree(scoped.path)
            else:
                scoped.path.unlink()
            return {
                "currentState": {"root": root, "path": scoped.relative, "exists": False},
                "actualChanges": [{"kind": "path", "action": "deleted", "path": scoped.relative, "beforeSha256": before_hash}],
            }

    def search(
        self,
        root: str,
        path: str,
        pattern: str,
        *,
        file_pattern: str = "*",
        case_sensitive: bool = True,
        limit: int = 200,
    ) -> dict[str, Any]:
        scoped = self.resolve(root, path, must_exist=True)
        try:
            expression = re.compile(pattern, 0 if case_sensitive else re.IGNORECASE)
        except re.error as error:
            raise ToolError("artifact.invalid_pattern", str(error), stage="files.search", context={"pattern": pattern}) from error
        limit = min(max(limit, 1), MAX_SEARCH_MATCHES)
        matches: list[dict[str, Any]] = []
        files = [scoped.path] if scoped.path.is_file() else [file for file in scoped.path.rglob(file_pattern) if self._is_within_root(file, scoped.root)]
        scanned = 0
        for file in files:
            if not file.is_file():
                continue
            scanned += 1
            try:
                with file.open("r", encoding="utf-8", errors="strict") as stream:
                    for line_number, line in enumerate(stream, 1):
                        for match in expression.finditer(line):
                            matches.append(
                                {
                                    "path": file.relative_to(scoped.root).as_posix(),
                                    "line": line_number,
                                    "column": match.start() + 1,
                                    "text": line.rstrip("\r\n")[:4096],
                                }
                            )
                            if len(matches) >= limit:
                                return {"matches": matches, "returned": len(matches), "truncated": True, "filesScanned": scanned}
            except (UnicodeDecodeError, OSError):
                continue
        return {"matches": matches, "returned": len(matches), "truncated": False, "filesScanned": scanned}

    def manifest(self, root: str, path: str = "", *, recursive: bool = True, limit: int = 5000) -> dict[str, Any]:
        scoped = self.resolve(root, path, must_exist=True)
        files = [scoped.path] if scoped.path.is_file() else [
            file
            for file in (scoped.path.rglob("*") if recursive else scoped.path.iterdir())
            if self._is_within_root(file, scoped.root)
        ]
        artifacts = []
        for file in sorted(files):
            if not file.is_file():
                continue
            stat = file.stat()
            artifacts.append(
                {"path": file.relative_to(scoped.root).as_posix(), "size": stat.st_size, "sha256": sha256_file(file), "modifiedNs": stat.st_mtime_ns}
            )
            if len(artifacts) >= limit:
                return {"root": root, "path": scoped.relative, "artifacts": artifacts, "truncated": True}
        return {"root": root, "path": scoped.relative, "artifacts": artifacts, "truncated": False}

    def compare(self, left_root: str, left_path: str, right_root: str, right_path: str, *, text: bool = False) -> dict[str, Any]:
        left = self.resolve(left_root, left_path, must_exist=True)
        right = self.resolve(right_root, right_path, must_exist=True)
        if not left.path.is_file() or not right.path.is_file():
            raise ToolError("artifact.not_file", "Both comparison paths must be files", stage="files.compare")
        left_hash = sha256_file(left.path)
        right_hash = sha256_file(right.path)
        result: dict[str, Any] = {
            "equal": left_hash == right_hash,
            "left": {"root": left_root, "path": left.relative, "sha256": left_hash, "size": left.path.stat().st_size},
            "right": {"root": right_root, "path": right.relative, "sha256": right_hash, "size": right.path.stat().st_size},
        }
        if text and left_hash != right_hash:
            left_lines = left.path.read_text(encoding="utf-8").splitlines()
            right_lines = right.path.read_text(encoding="utf-8").splitlines()
            result["diff"] = "\n".join(difflib.unified_diff(left_lines, right_lines, fromfile=left.relative, tofile=right.relative, lineterm=""))[:262144]
        return result

    def backup(self, name: str, items: list[dict[str, str]]) -> dict[str, Any]:
        archive_path = self._backup_archive_path(name, stage="files.backup")
        manifest: list[dict[str, Any]] = []
        with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for item in items:
                scoped = self.resolve(item["root"], item["path"], must_exist=True)
                paths = [scoped.path] if scoped.path.is_file() else [
                    file for file in scoped.path.rglob("*") if self._is_within_root(file, scoped.root)
                ]
                for file in paths:
                    if not file.is_file():
                        continue
                    relative = file.relative_to(scoped.root).as_posix()
                    archive_name = f"files/{scoped.root_name}/{relative}"
                    archive.write(file, archive_name)
                    manifest.append({"root": scoped.root_name, "path": relative, "sha256": sha256_file(file), "size": file.stat().st_size})
            metadata = {"name": name, "createdAt": utc_now(), "files": manifest}
            archive.writestr("manifest.json", json.dumps(metadata, ensure_ascii=False, indent=2))
        digest = sha256_file(archive_path)
        return {
            "currentState": {"name": name, "path": archive_path.as_posix(), "sha256": digest, "files": len(manifest)},
            "actualChanges": [{"kind": "backup", "action": "created", "path": archive_path.as_posix()}],
            "evidence": [{"kind": "artifact_hash", "source": archive_path.as_posix(), "sha256": digest}],
        }

    def restore(self, name: str, *, overwrite: bool = False) -> dict[str, Any]:
        archive_path = self._backup_archive_path(name, stage="files.restore")
        if not archive_path.is_file():
            raise ToolError("artifact.backup_not_found", f"Backup '{name}' was not found", stage="files.restore")
        restored: list[dict[str, Any]] = []
        conflicts: list[dict[str, Any]] = []
        with zipfile.ZipFile(archive_path) as archive:
            metadata = json.loads(archive.read("manifest.json"))
            for entry in metadata["files"]:
                scoped = self.resolve(entry["root"], entry["path"])
                if scoped.path.exists() and not overwrite:
                    actual_hash = sha256_file(scoped.path) if scoped.path.is_file() else None
                    if actual_hash != entry["sha256"]:
                        conflicts.append({"root": entry["root"], "path": entry["path"], "actualSha256": actual_hash})
                    continue
                scoped.path.parent.mkdir(parents=True, exist_ok=True)
                data = archive.read(f"files/{entry['root']}/{entry['path']}")
                scoped.path.write_bytes(data)
                restored.append({"root": entry["root"], "path": entry["path"], "sha256": sha256_file(scoped.path)})
        status = "partial" if conflicts else "succeeded"
        return {
            "status": status,
            "currentState": {"name": name, "restored": restored, "conflicts": conflicts},
            "actualChanges": [{"kind": "file", "action": "restored", **item} for item in restored],
        }

    def _mutation_lock(self, scoped: ScopedPath) -> threading.RLock:
        return self._mutation_locks[hash(scoped.path) % len(self._mutation_locks)]

    def _backup_archive_path(self, name: str, *, stage: str) -> Path:
        if not re.fullmatch(r"[A-Za-z0-9_.-]{1,80}", name):
            raise ToolError("artifact.invalid_backup_name", "Backup name contains unsupported characters", stage=stage)
        backup_directory = self.backup_directory.resolve()
        archive_path = (backup_directory / f"{name}.zip").resolve()
        try:
            archive_path.relative_to(backup_directory)
        except ValueError as error:
            raise ToolError(
                "artifact.invalid_backup_name",
                "Backup archive resolves outside the backup directory",
                stage=stage,
                status="rejected",
                context={"name": name},
            ) from error
        return archive_path


    @staticmethod
    def _is_within_root(path: Path, root: Path) -> bool:
        try:
            path.resolve(strict=False).relative_to(root)
            return True
        except (OSError, ValueError):
            return False
