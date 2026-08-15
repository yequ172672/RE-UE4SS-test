from __future__ import annotations
from contextlib import contextmanager

from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat
import tempfile
from typing import Any

from .model import ToolError
from .workspace import ScopedPath, Workspace


MAX_INI_BYTES = 4 * 1024 * 1024
MAX_SECTIONS = 1024
MAX_KEYS = 16384
MAX_ENTRIES = 32768
MAX_CHANGES = 100

_SECTION_RE = re.compile(r"^ *\[([^\]\r\n=;]+)\] *$")
_ENTRY_RE = re.compile(r"^( *)([^=\r\n]+?)( *= *)(.*)$")
_SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")


@dataclass(frozen=True, slots=True)
class _Entry:
    section: str
    key: str
    value: str
    line_index: int
    value_start: int
    value_end: int


@dataclass(slots=True)
class _Document:
    raw: bytes
    text: str
    has_bom: bool
    newline: str
    lines: list[str]
    sections: dict[str, dict[str, str]]
    section_names: dict[str, str]
    key_names: dict[tuple[str, str], str]
    entries: list[_Entry]
    section_lines: dict[str, int]
    def value(self, section: str, key: str) -> str | None:
        return self.sections.get(section, {}).get(key)

    def public_sections(self) -> dict[str, dict[str, str]]:
        result: dict[str, dict[str, str]] = {}
        for section, values in self.sections.items():
            result[section] = {
                self.key_names[(section, key)]: value
                for key, value in values.items()
            }
        return result


class SettingsManager:
    def __init__(self, workspace: Workspace) -> None:
        self.workspace = workspace

    def profile(self, root: str, path: str, override_path: str | None = None) -> dict[str, Any]:
        base_scoped = self.workspace.resolve(root, path, must_exist=True)
        self._reject_same_override(root, base_scoped, override_path)
        base = self._read_document(base_scoped, role="base")
        override_scoped, override = self._read_override(root, override_path)
        state = self._profile_state(root, base_scoped, base, override_scoped, override)
        return {
            "currentState": state,
            "evidence": self._hash_evidence(base_scoped, base, override_scoped, override),
        }

    def patch(
        self,
        root: str,
        path: str,
        changes: Any,
        expected_sha256: str,
        override_path: str | None = None,
    ) -> dict[str, Any]:
        normalized = self._normalize_changes(changes)
        if not isinstance(expected_sha256, str) or not _SHA256_RE.fullmatch(expected_sha256):
            raise ToolError(
                "settings.expected_hash_required",
                "expectedSha256 must be a 64-character hexadecimal SHA-256 hash",
                stage="settings.patch.validate",
                status="rejected",
            )

        scoped = self.workspace.resolve(root, path, must_exist=True)
        self._reject_same_override(root, scoped, override_path)
        with self._process_lock(scoped.path), self.workspace._mutation_lock(scoped):
            base = self._read_document(scoped, role="base")
            before_hash = hashlib.sha256(base.raw).hexdigest()
            if before_hash != expected_sha256.lower():
                raise ToolError(
                    "artifact.conflict",
                    "The settings file changed after it was profiled",
                    stage="settings.patch.conflict",
                    context={
                        "root": root,
                        "path": scoped.relative,
                        "expectedSha256": expected_sha256,
                        "actualSha256": before_hash,
                    },
                    retryable=True,
                    suggested_actions=["Read a new settings profile and reapply the intended changes"],
                    status="rejected",
                )
            override_scoped, override = self._read_override(root, override_path)

            before_values = {
                (change["section"], change["key"]): base.value(change["section"], change["key"])
                for change in normalized
            }
            patched_text = self._patch_text(base, normalized)
            patched_raw = ((b"\xef\xbb\xbf" if base.has_bom else b"") + patched_text.encode("utf-8"))
            if len(patched_raw) > MAX_INI_BYTES:
                raise ToolError(
                    "settings.file_too_large",
                    "The patched INI would exceed the 4 MiB limit",
                    stage="settings.patch.validate",
                    context={"path": scoped.relative, "size": len(patched_raw), "maxBytes": MAX_INI_BYTES},
                    status="rejected",
                )
            candidate = self._parse(patched_raw, scoped.relative, role="patched")
            for index, change in enumerate(normalized):
                parsed_value = candidate.value(change["section"], change["key"])
                if parsed_value != change["value"]:
                    raise ToolError(
                        "settings.invalid_change",
                        "The requested value cannot be represented unambiguously in the INI",
                        stage="settings.patch.validate",
                        context={"changeIndex": index, "section": change["section"], "key": change["key"], "requestedValue": change["value"], "parsedValue": parsed_value},
                        status="rejected",
                    )

            if patched_raw != base.raw:
                self._atomic_replace(scoped.path, patched_raw)

            verified = self._read_document(scoped, role="base")
            after_hash = hashlib.sha256(verified.raw).hexdigest()
            if verified.raw != patched_raw:
                raise ToolError(
                    "settings.write_verification_failed",
                    "The settings file did not match the atomic patch after write",
                    stage="settings.patch.verify",
                    context={"path": scoped.relative, "expectedSha256": hashlib.sha256(patched_raw).hexdigest(), "actualSha256": after_hash},
                )
            for change in normalized:
                actual = verified.value(change["section"], change["key"])
                if actual != change["value"]:
                    raise ToolError(
                        "settings.write_verification_failed",
                        "A patched setting did not read back with the requested value",
                        stage="settings.patch.verify",
                        context={"path": scoped.relative, "section": change["section"], "key": change["key"], "expectedValue": change["value"], "actualValue": actual},
                    )

            actual_changes = []
            for change in normalized:
                identity = (change["section"], change["key"])
                override_value = override.value(change["section"], change["key"]) if override else None
                shadowed = override_value is not None
                actual_changes.append(
                    {
                        "kind": "ue4ss_setting",
                        "section": change["section"],
                        "key": change["key"],
                        "before": before_values[identity],
                        "after": change["value"],
                        "effectiveValue": override_value if shadowed else change["value"],
                        "shadowedByOverride": shadowed,
                    }
                )

            state = self._profile_state(root, scoped, verified, override_scoped, override)
            state["verified"] = True
            return {
                "currentState": state,
                "actualChanges": actual_changes,
                "evidence": [
                    {
                        "kind": "settings_patch_hash",
                        "source": scoped.path.as_posix(),
                        "beforeSha256": before_hash,
                        "afterSha256": after_hash,
                        "size": len(verified.raw),
                        "readBackVerified": True,
                    },
                    *self._override_hash_evidence(override_scoped, override),
                ],
                "restartRequired": True,
            }

    @contextmanager
    def _process_lock(self, path: Path) -> Any:
        normalized_path = str(path.resolve()).casefold() if os.name == "nt" else str(path.resolve())
        lock_name = hashlib.sha256(normalized_path.encode("utf-8")).hexdigest() + ".lock"
        lock_directory = Path(tempfile.gettempdir()) / "ue4ss-mcp-locks"
        try:
            lock_directory.mkdir(parents=True, exist_ok=True)
            with (lock_directory / lock_name).open("a+b") as lock:
                lock.seek(0, os.SEEK_END)
                if lock.tell() == 0:
                    lock.write(b"\0")
                    lock.flush()
                lock.seek(0)
                if os.name == "nt":
                    import msvcrt

                    msvcrt.locking(lock.fileno(), msvcrt.LK_LOCK, 1)
                    try:
                        yield
                    finally:
                        lock.seek(0)
                        msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    import fcntl

                    fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
                    try:
                        yield
                    finally:
                        fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
        except ToolError:
            raise
        except OSError as error:
            raise ToolError(
                "settings.lock_failed",
                str(error),
                stage="settings.patch.lock",
                context={"path": path.as_posix(), "exceptionType": type(error).__name__},
                retryable=True,
                suggested_actions=["Retry after the other settings writer completes"],
            ) from error

    def _reject_same_override(self, root: str, base: ScopedPath, override_path: str | None) -> None:
        if override_path is None:
            return
        override = self.workspace.resolve(root, override_path)
        if override.path == base.path:
            raise ToolError(
                "settings.override_matches_base",
                "overridePath must identify a different read-only file from path",
                stage="settings.validate",
                context={"root": root, "path": base.relative, "overridePath": override.relative},
                status="rejected",
            )

    def _read_override(self, root: str, override_path: str | None) -> tuple[ScopedPath | None, _Document | None]:
        if override_path is None:
            return None, None
        scoped = self.workspace.resolve(root, override_path)
        if not scoped.path.exists():
            return scoped, None
        return scoped, self._read_document(scoped, role="override")

    def _read_document(self, scoped: ScopedPath, *, role: str) -> _Document:
        if not scoped.path.is_file():
            raise ToolError(
                "artifact.not_file",
                f"The {role} settings path is not a file",
                stage="settings.read",
                context={"path": scoped.relative, "role": role},
            )
        try:
            size = scoped.path.stat().st_size
            if size > MAX_INI_BYTES:
                raise ToolError(
                    "settings.file_too_large",
                    "The INI exceeds the 4 MiB limit",
                    stage="settings.read",
                    context={"path": scoped.relative, "role": role, "size": size, "maxBytes": MAX_INI_BYTES},
                )
            with scoped.path.open("rb") as stream:
                raw = stream.read(MAX_INI_BYTES + 1)
        except ToolError:
            raise
        except OSError as error:
            raise ToolError(
                "settings.read_failed",
                str(error),
                stage="settings.read",
                context={"path": scoped.relative, "role": role, "exceptionType": type(error).__name__},
            ) from error
        if len(raw) > MAX_INI_BYTES:
            raise ToolError(
                "settings.file_too_large",
                "The INI exceeds the 4 MiB limit",
                stage="settings.read",
                context={"path": scoped.relative, "role": role, "size": len(raw), "maxBytes": MAX_INI_BYTES},
            )
        return self._parse(raw, scoped.relative, role=role)

    def _parse(self, raw: bytes, path: str, *, role: str) -> _Document:
        has_bom = raw.startswith(b"\xef\xbb\xbf")
        payload = raw[3:] if has_bom else raw
        try:
            text = payload.decode("utf-8")
        except UnicodeDecodeError as error:
            raise ToolError(
                "settings.invalid_encoding",
                "UE4SS settings INI must be UTF-8, optionally with a BOM",
                stage="settings.parse",
                context={"path": path, "role": role, "byteOffset": error.start, "encoding": "utf-8"},
            ) from error

        lines = text.splitlines(keepends=True)
        if text and not lines:
            lines = [text]
        newline_match = re.search(r"\r\n|\n|\r", text)
        newline = newline_match.group(0) if newline_match else "\n"
        sections: dict[str, dict[str, str]] = {}
        section_names: dict[str, str] = {}
        key_names: dict[tuple[str, str], str] = {}
        entries: list[_Entry] = []
        section_lines: dict[str, int] = {}
        current_section: str | None = None
        distinct_keys: set[tuple[str, str]] = set()

        for line_index, line in enumerate(lines):
            content = line.rstrip("\r\n")
            stripped = content.strip(" ")
            if not stripped or content.lstrip(" ").startswith(";"):
                continue
            section_match = _SECTION_RE.fullmatch(content)
            if section_match:
                section = section_match.group(1)
                if not section.strip():
                    self._parse_error(path, role, line_index, "Section names cannot be empty", content)
                if section not in sections:
                    if len(sections) >= MAX_SECTIONS:
                        self._limit_error(path, role, "sections", MAX_SECTIONS)
                    sections[section] = {}
                    section_names[section] = section
                    section_lines[section] = line_index
                current_section = section
                continue
            entry_match = _ENTRY_RE.fullmatch(content)
            if not entry_match:
                self._parse_error(path, role, line_index, "Expected a [section] header or key=value entry", content)
            if current_section is None:
                self._parse_error(path, role, line_index, "Setting entries must appear inside a section", content)
            key = entry_match.group(2).rstrip(" ")
            if not key:
                self._parse_error(path, role, line_index, "Setting keys cannot be empty", content)
            if any(character in key for character in "[]; "):
                self._parse_error(path, role, line_index, "Setting keys cannot contain '[', ']', ';' or spaces", content)
            value_text = entry_match.group(4)
            value = value_text.lstrip(" ")
            identity = (current_section, key)
            distinct_keys.add(identity)
            if len(distinct_keys) > MAX_KEYS:
                self._limit_error(path, role, "keys", MAX_KEYS)
            if len(entries) >= MAX_ENTRIES:
                self._limit_error(path, role, "entries", MAX_ENTRIES)
            value_offset = entry_match.start(4)
            leading = len(value_text) - len(value)
            entries.append(
                _Entry(
                    section=current_section,
                    key=key,
                    value=value,
                    line_index=line_index,
                    value_start=value_offset + leading,
                    value_end=value_offset + len(value_text),
                )
            )
            sections[current_section][key] = value
            key_names[identity] = key

        return _Document(raw, text, has_bom, newline, lines, sections, section_names, key_names, entries, section_lines)


    @staticmethod
    def _parse_error(path: str, role: str, line_index: int, message: str, line: str) -> None:
        raise ToolError(
            "settings.parse_error",
            message,
            stage="settings.parse",
            context={"path": path, "role": role, "line": line_index + 1, "linePreview": line[:256]},
        )

    @staticmethod
    def _limit_error(path: str, role: str, kind: str, maximum: int) -> None:
        raise ToolError(
            "settings.parse_limit_exceeded",
            f"The INI exceeds the {kind} limit",
            stage="settings.parse",
            context={"path": path, "role": role, "limitKind": kind, "maximum": maximum},
        )

    def _normalize_changes(self, changes: Any) -> list[dict[str, str]]:
        if not isinstance(changes, list) or not 1 <= len(changes) <= MAX_CHANGES:
            raise ToolError(
                "settings.invalid_changes",
                "changes must contain between 1 and 100 entries",
                stage="settings.patch.validate",
                context={"maximum": MAX_CHANGES},
                status="rejected",
            )
        normalized: list[dict[str, str]] = []
        seen: set[tuple[str, str]] = set()
        for index, change in enumerate(changes):
            if not isinstance(change, dict):
                self._change_error(index, "Each change must be an object")
            section = change.get("section")
            key = change.get("key")
            if not isinstance(section, str) or not section.strip():
                self._change_error(index, "section must be a non-empty string")
            if not isinstance(key, str) or not key.strip():
                self._change_error(index, "key must be a non-empty string")
            if not section.strip():
                self._change_error(index, "section must contain a non-whitespace character")
            if any(character in section for character in "\r\n[]=;"):
                self._change_error(index, "section cannot contain '[', ']', '=' or ';'")
            if any(character in key for character in "\r\n=[]; "):
                self._change_error(index, "key cannot contain '[', ']', '=', ';' or spaces")
            if key.startswith(("+", "-")):
                self._change_error(index, "directive keys beginning with '+' or '-' cannot be patched")
            if "value" not in change:
                self._change_error(index, "value is required")
            value = self._serialize_value(change["value"], index)
            identity = (section, key)
            if identity in seen:
                self._change_error(index, "duplicate section/key change")
            seen.add(identity)
            normalized.append({"section": section, "key": key, "value": value})
        return normalized

    def _serialize_value(self, value: Any, index: int) -> str:
        if isinstance(value, str):
            if "\r" in value or "\n" in value:
                self._change_error(index, "value cannot contain newlines")
            return value
        if isinstance(value, bool):
            return "true" if value else "false"
        if isinstance(value, int):
            return str(value)
        if isinstance(value, float):
            if not math.isfinite(value):
                self._change_error(index, "numeric value must be finite")
            return json.dumps(value, ensure_ascii=False, allow_nan=False)
        self._change_error(index, "value must be a string, boolean, integer, or number")
        raise AssertionError("unreachable")

    @staticmethod
    def _change_error(index: int, message: str) -> None:
        raise ToolError(
            "settings.invalid_change",
            message,
            stage="settings.patch.validate",
            context={"changeIndex": index},
            status="rejected",
        )

    def _patch_text(self, document: _Document, changes: list[dict[str, str]]) -> str:
        replacements: dict[int, tuple[int, int, str]] = {}
        pending: dict[str, list[dict[str, str]]] = {}
        last_entries: dict[tuple[str, str], _Entry] = {}
        for entry in document.entries:
            last_entries[(entry.section, entry.key)] = entry

        for change in changes:
            identity = (change["section"], change["key"])
            entry = last_entries.get(identity)
            if entry is not None:
                replacements[entry.line_index] = (entry.value_start, entry.value_end, change["value"])
            else:
                pending.setdefault(identity[0], []).append(change)

        lines = list(document.lines)
        for line_index, (start, end, value) in replacements.items():
            lines[line_index] = lines[line_index][:start] + value + lines[line_index][end:]

        if not pending:
            return "".join(lines)

        insertions: dict[int, list[str]] = {}
        new_section_blocks: list[str] = []
        section_order = sorted(document.section_lines.items(), key=lambda item: item[1])
        for section, section_changes in pending.items():
            if section in document.section_lines:
                section_line = document.section_lines[section]
                next_headers = [line for _, line in section_order if line > section_line]
                insertion_line = min(next_headers) if next_headers else len(lines)
                insertions.setdefault(insertion_line, []).extend(
                    f"{change['key']} = {change['value']}{document.newline}" for change in section_changes
                )
            else:
                if lines and not self._line_has_ending(lines[-1]) and not new_section_blocks:
                    new_section_blocks.append(document.newline)
                new_section_blocks.append(f"[{section_changes[0]['section']}]{document.newline}")
                new_section_blocks.extend(f"{change['key']} = {change['value']}{document.newline}" for change in section_changes)

        if new_section_blocks:
            insertions.setdefault(len(lines), []).extend(new_section_blocks)

        result: list[str] = []
        for index in range(len(lines) + 1):
            addition = insertions.get(index)
            if addition:
                if result and not self._line_has_ending(result[-1]):
                    result.append(document.newline)
                result.extend(addition)
            if index < len(lines):
                result.append(lines[index])
        return "".join(result)

    @staticmethod
    def _line_has_ending(line: str) -> bool:
        return line.endswith(("\r", "\n"))

    @staticmethod
    def _atomic_replace(path: Path, data: bytes) -> None:
        mode = stat.S_IMODE(path.stat().st_mode)
        temporary_path: Path | None = None
        try:
            with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as temporary:
                temporary_path = Path(temporary.name)
                temporary.write(data)
                temporary.flush()
                os.fsync(temporary.fileno())
            os.chmod(temporary_path, mode)
            os.replace(temporary_path, path)
            temporary_path = None
        except OSError as error:
            raise ToolError(
                "settings.write_failed",
                str(error),
                stage="settings.patch.write",
                context={"path": path.as_posix(), "exceptionType": type(error).__name__},
            ) from error
        finally:
            if temporary_path is not None:
                temporary_path.unlink(missing_ok=True)

    def _profile_state(
        self,
        root: str,
        base_scoped: ScopedPath,
        base: _Document,
        override_scoped: ScopedPath | None,
        override: _Document | None,
    ) -> dict[str, Any]:
        effective, sources = self._effective_sections(base, override, override_scoped)
        return {
            "root": root,
            "path": base_scoped.relative,
            "baseSha256": hashlib.sha256(base.raw).hexdigest(),
            "baseSize": len(base.raw),
            "baseHasUtf8Bom": base.has_bom,
            "baseSections": base.public_sections(),
            "override": {
                "path": override_scoped.relative if override_scoped else None,
                "exists": override is not None,
                "sha256": hashlib.sha256(override.raw).hexdigest() if override else None,
                "size": len(override.raw) if override else None,
                "hasUtf8Bom": override.has_bom if override else None,
            },
            "effectiveSections": effective,
            "overrideSources": sources,
            "limits": {"maxBytes": MAX_INI_BYTES, "maxSections": MAX_SECTIONS, "maxKeys": MAX_KEYS, "maxEntries": MAX_ENTRIES},
        }

    def _effective_sections(
        self,
        base: _Document,
        override: _Document | None,
        override_scoped: ScopedPath | None,
    ) -> tuple[dict[str, dict[str, str]], list[dict[str, Any]]]:
        effective = {section: dict(values) for section, values in base.public_sections().items()}
        if override is None:
            return effective, []
        sources: list[dict[str, Any]] = []
        for override_section, values in override.public_sections().items():
            if override_section not in effective:
                effective[override_section] = {}
            for override_key, value in values.items():
                base_value = effective[override_section].get(override_key)
                effective[override_section][override_key] = value
                sources.append(
                    {
                        "section": override_section,
                        "key": override_key,
                        "value": value,
                        "baseValue": base_value,
                        "source": "override",
                        "sourcePath": override_scoped.relative if override_scoped else None,
                    }
                )
        return effective, sources

    def _hash_evidence(
        self,
        base_scoped: ScopedPath,
        base: _Document,
        override_scoped: ScopedPath | None,
        override: _Document | None,
    ) -> list[dict[str, Any]]:
        return [
            {"kind": "settings_hash", "source": base_scoped.path.as_posix(), "role": "base", "sha256": hashlib.sha256(base.raw).hexdigest(), "size": len(base.raw)},
            *self._override_hash_evidence(override_scoped, override),
        ]

    @staticmethod
    def _override_hash_evidence(override_scoped: ScopedPath | None, override: _Document | None) -> list[dict[str, Any]]:
        if override_scoped is None:
            return []
        evidence: dict[str, Any] = {
            "kind": "settings_hash",
            "source": override_scoped.path.as_posix(),
            "role": "override",
            "exists": override is not None,
        }
        if override is not None:
            evidence.update({"sha256": hashlib.sha256(override.raw).hexdigest(), "size": len(override.raw)})
        return [evidence]
