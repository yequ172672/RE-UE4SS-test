from __future__ import annotations

import base64
import binascii
import ctypes
import hashlib
import os
import platform
from typing import Any, Iterator

from .model import ToolError


MAX_TRANSFER_BYTES = 1024 * 1024
MAX_PATTERN_BYTES = 4096
MAX_SCAN_BYTES = 64 * 1024 * 1024
MAX_MATCHES = 1000
MAX_REGIONS = 1000
_MAX_SCAN_REGIONS = MAX_SCAN_BYTES // 4096 + 2

_PROCESS_QUERY_INFORMATION = 0x0400
_PROCESS_VM_OPERATION = 0x0008
_PROCESS_VM_READ = 0x0010
_PROCESS_VM_WRITE = 0x0020
_MEM_COMMIT = 0x1000
_PAGE_GUARD = 0x100
_PAGE_NOACCESS = 0x01
_READABLE_PROTECTIONS = {0x02, 0x04, 0x08, 0x20, 0x40, 0x80}
_WRITABLE_PROTECTIONS = {0x04, 0x08, 0x40, 0x80}
_PROTECTION_NAMES = {
    0x01: "no_access",
    0x02: "read_only",
    0x04: "read_write",
    0x08: "write_copy",
    0x10: "execute",
    0x20: "execute_read",
    0x40: "execute_read_write",
    0x80: "execute_write_copy",
}
_STATE_NAMES = {0x1000: "commit", 0x2000: "reserve", 0x10000: "free"}
_TYPE_NAMES = {0x20000: "private", 0x40000: "mapped", 0x1000000: "image"}


if os.name == "nt":
    from ctypes import wintypes

    class _MemoryBasicInformation(ctypes.Structure):
        _fields_ = [
            ("BaseAddress", ctypes.c_void_p),
            ("AllocationBase", ctypes.c_void_p),
            ("AllocationProtect", wintypes.DWORD),
            ("PartitionId", wintypes.WORD),
            ("RegionSize", ctypes.c_size_t),
            ("State", wintypes.DWORD),
            ("Protect", wintypes.DWORD),
            ("Type", wintypes.DWORD),
        ]

    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _OpenProcess = _kernel32.OpenProcess
    _OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    _OpenProcess.restype = wintypes.HANDLE
    _CloseHandle = _kernel32.CloseHandle
    _CloseHandle.argtypes = [wintypes.HANDLE]
    _CloseHandle.restype = wintypes.BOOL
    _VirtualQueryEx = _kernel32.VirtualQueryEx
    _VirtualQueryEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(_MemoryBasicInformation), ctypes.c_size_t]
    _VirtualQueryEx.restype = ctypes.c_size_t
    _ReadProcessMemory = _kernel32.ReadProcessMemory
    _ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    _ReadProcessMemory.restype = wintypes.BOOL
    _WriteProcessMemory = _kernel32.WriteProcessMemory
    _WriteProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    _WriteProcessMemory.restype = wintypes.BOOL


class _ProcessHandle:
    def __init__(self, process_id: int, access: int, stage: str) -> None:
        self.process_id = process_id
        self.access = access
        self.stage = stage
        self.handle: Any = None

    def __enter__(self) -> Any:
        ctypes.set_last_error(0)
        self.handle = _OpenProcess(self.access, False, self.process_id)
        if not self.handle:
            error = ctypes.get_last_error()
            code = "memory.access_denied" if error == 5 else "memory.process_not_found" if error == 87 else "memory.open_failed"
            raise ToolError(
                code,
                _windows_error_message(error, "OpenProcess failed"),
                stage=self.stage,
                context={"processId": self.process_id, "access": self.access, "winError": error},
            )
        return self.handle

    def __exit__(self, exc_type: object, exc_value: object, traceback: object) -> None:
        if self.handle:
            _CloseHandle(self.handle)
            self.handle = None


def _windows_error_message(error: int, fallback: str) -> str:
    if error:
        try:
            return ctypes.FormatError(error).strip()
        except OSError:
            pass
    return fallback


def _require_windows(stage: str) -> None:
    if os.name != "nt":
        raise ToolError(
            "capability.unavailable",
            "Windows process memory operations are unavailable on this platform",
            stage=stage,
            context={"platform": platform.system(), "osName": os.name},
        )


def _validate_process_id(process_id: int, stage: str) -> None:
    if isinstance(process_id, bool) or not isinstance(process_id, int) or process_id <= 0 or process_id > 0xFFFFFFFF:
        raise ToolError("memory.invalid_process_id", "processId must be an integer from 1 through 4294967295", stage=stage, context={"processId": process_id})


def _validate_address(address: int, stage: str) -> None:
    maximum = (1 << (ctypes.sizeof(ctypes.c_void_p) * 8)) - 1
    if isinstance(address, bool) or not isinstance(address, int) or address < 0 or address > maximum:
        raise ToolError("memory.invalid_address", "address must be a non-negative native pointer value", stage=stage, context={"address": address, "maximum": maximum})


def _validate_size(size: int, maximum: int, stage: str, *, allow_zero: bool = False) -> None:
    minimum = 0 if allow_zero else 1
    if isinstance(size, bool) or not isinstance(size, int) or size < minimum or size > maximum:
        raise ToolError("memory.invalid_size", f"size must be between {minimum} and {maximum} bytes", stage=stage, context={"size": size, "minimum": minimum, "maximum": maximum})


def _ensure_address_range(address: int, size: int, stage: str) -> int:
    _validate_address(address, stage)
    maximum = (1 << (ctypes.sizeof(ctypes.c_void_p) * 8)) - 1
    if size > maximum - address:
        raise ToolError("memory.invalid_address_range", "address plus size exceeds the native address space", stage=stage, context={"address": address, "size": size, "maximum": maximum})
    return address + size


def _protection_details(protect: int) -> tuple[str, bool, bool, bool]:
    base = protect & 0xFF
    guarded = bool(protect & _PAGE_GUARD)
    readable = base in _READABLE_PROTECTIONS and not guarded and base != _PAGE_NOACCESS
    writable = base in _WRITABLE_PROTECTIONS and not guarded and base != _PAGE_NOACCESS
    return _PROTECTION_NAMES.get(base, f"unknown_0x{base:x}"), readable, writable, guarded


def _query_regions(handle: Any, start_address: int, size: int, max_regions: int, stage: str, *, fail_on_limit: bool = False) -> Iterator[dict[str, Any]]:
    end_address = _ensure_address_range(start_address, size, stage)
    current = start_address
    emitted = 0
    while current < end_address and emitted < max_regions:
        information = _MemoryBasicInformation()
        ctypes.set_last_error(0)
        result = _VirtualQueryEx(handle, ctypes.c_void_p(current), ctypes.byref(information), ctypes.sizeof(information))
        if not result:
            error = ctypes.get_last_error()
            raise ToolError(
                "memory.query_failed",
                _windows_error_message(error, "VirtualQueryEx failed"),
                stage=stage,
                context={"address": current, "winError": error},
            )
        base_address = int(information.BaseAddress or 0)
        region_size = int(information.RegionSize)
        if region_size <= 0 or base_address > current or base_address + region_size <= current:
            raise ToolError(
                "memory.invalid_region",
                "VirtualQueryEx returned a non-advancing memory region",
                stage=stage,
                context={"address": current, "baseAddress": base_address, "regionSize": region_size},
            )
        protection_name, readable, writable, guarded = _protection_details(int(information.Protect))
        intersection_start = max(current, base_address)
        intersection_end = min(end_address, base_address + region_size)
        yield {
            "baseAddress": base_address,
            "allocationBase": int(information.AllocationBase or 0),
            "regionSize": region_size,
            "rangeStart": intersection_start,
            "rangeSize": intersection_end - intersection_start,
            "state": _STATE_NAMES.get(int(information.State), f"unknown_0x{int(information.State):x}"),
            "protection": protection_name,
            "protectionValue": int(information.Protect),
            "type": _TYPE_NAMES.get(int(information.Type), f"unknown_0x{int(information.Type):x}"),
            "readable": bool(information.State == _MEM_COMMIT and readable),
            "writable": bool(information.State == _MEM_COMMIT and writable),
            "guarded": guarded,
        }
        emitted += 1
        current = base_address + region_size
    if fail_on_limit and current < end_address:
        raise ToolError(
            "memory.region_limit_exceeded",
            "The address range contains more memory regions than the bounded scan can inspect",
            stage=stage,
            context={"startAddress": start_address, "size": size, "scannedUntilAddress": current, "maxRegions": max_regions},
            status="partial",
        )


def _read_with_handle(handle: Any, process_id: int, address: int, size: int, stage: str) -> bytes:
    buffer = (ctypes.c_ubyte * size)()
    transferred = ctypes.c_size_t()
    ctypes.set_last_error(0)
    success = _ReadProcessMemory(handle, ctypes.c_void_p(address), buffer, size, ctypes.byref(transferred))
    actual = int(transferred.value)
    if not success or actual != size:
        error = ctypes.get_last_error()
        code = "memory.partial_read" if actual else "memory.read_failed"
        if error == 5:
            code = "memory.access_denied"
        raise ToolError(
            code,
            _windows_error_message(error, "ReadProcessMemory did not read the requested range"),
            stage=stage,
            context={"processId": process_id, "address": address, "requestedBytes": size, "bytesRead": actual, "winError": error},
        )
    return bytes(buffer)


def _parse_pattern(pattern: str) -> tuple[bytes, bytes]:
    if not isinstance(pattern, str):
        raise ToolError("memory.invalid_pattern", "pattern must be a hexadecimal token string", stage="memory.pattern.parse", context={"type": type(pattern).__name__})
    tokens = pattern.split()
    if not tokens or len(tokens) > MAX_PATTERN_BYTES:
        raise ToolError(
            "memory.invalid_pattern",
            f"pattern must contain between 1 and {MAX_PATTERN_BYTES} byte tokens",
            stage="memory.pattern.parse",
            context={"tokenCount": len(tokens), "maximum": MAX_PATTERN_BYTES},
        )
    values = bytearray()
    masks = bytearray()
    for index, token in enumerate(tokens):
        if token == "??":
            values.append(0)
            masks.append(0)
            continue
        if len(token) != 2:
            raise ToolError("memory.invalid_pattern", "Each pattern token must be two hexadecimal digits or ??", stage="memory.pattern.parse", context={"token": token, "tokenIndex": index})
        try:
            values.append(int(token, 16))
        except ValueError as error:
            raise ToolError("memory.invalid_pattern", "Each pattern token must be two hexadecimal digits or ??", stage="memory.pattern.parse", context={"token": token, "tokenIndex": index}) from error
        masks.append(0xFF)
    return bytes(values), bytes(masks)


def _matching_offsets(data: bytes, values: bytes, masks: bytes, limit: int) -> Iterator[int]:
    length = len(values)
    if len(data) < length or limit <= 0:
        return
    fixed_indexes = [index for index, mask in enumerate(masks) if mask]
    if not fixed_indexes:
        yield from range(min(len(data) - length + 1, limit))
        return
    runs: list[tuple[int, int]] = []
    run_start = fixed_indexes[0]
    run_end = run_start + 1
    for index in fixed_indexes[1:]:
        if index == run_end:
            run_end += 1
        else:
            runs.append((run_start, run_end))
            run_start = index
            run_end = index + 1
    runs.append((run_start, run_end))
    anchor_start, anchor_end = max(runs, key=lambda item: item[1] - item[0])
    needle = values[anchor_start:anchor_end]
    search_from = anchor_start
    yielded = 0
    while yielded < limit:
        found = data.find(needle, search_from)
        if found < 0:
            return
        candidate = found - anchor_start
        search_from = found + 1
        if candidate < 0 or candidate + length > len(data):
            continue
        if all(data[candidate + index] == values[index] for index in fixed_indexes):
            yield candidate
            yielded += 1


class MemoryManager:
    def profile(self) -> dict[str, Any]:
        available = os.name == "nt"
        return {
            "available": available,
            "platform": platform.system(),
            "architecture": platform.machine(),
            "pointerBits": ctypes.sizeof(ctypes.c_void_p) * 8,
            "supportedOperations": ["regions", "read", "write", "patternScan"] if available else [],
            "limits": {
                "maxTransferBytes": MAX_TRANSFER_BYTES,
                "maxPatternBytes": MAX_PATTERN_BYTES,
                "maxScanBytes": MAX_SCAN_BYTES,
                "maxMatches": MAX_MATCHES,
                "maxRegions": MAX_REGIONS,
            },
            "missingConditions": [] if available else ["Windows is required for process memory operations"],
        }

    def regions(self, process_id: int, start_address: int, size: int, max_regions: int = MAX_REGIONS) -> dict[str, Any]:
        stage = "memory.regions"
        _require_windows(stage)
        _validate_process_id(process_id, stage)
        _validate_size(size, MAX_SCAN_BYTES, stage)
        _validate_size(max_regions, MAX_REGIONS, stage)
        with _ProcessHandle(process_id, _PROCESS_QUERY_INFORMATION, stage) as handle:
            regions = list(_query_regions(handle, start_address, size, max_regions, stage))
        covered = sum(region["rangeSize"] for region in regions)
        return {
            "processId": process_id,
            "startAddress": start_address,
            "size": size,
            "regions": regions,
            "regionCount": len(regions),
            "coveredBytes": covered,
            "truncated": covered < size and len(regions) >= max_regions,
        }

    def read(self, process_id: int, address: int, size: int) -> dict[str, Any]:
        stage = "memory.read"
        _require_windows(stage)
        _validate_process_id(process_id, stage)
        _validate_size(size, MAX_TRANSFER_BYTES, stage)
        _ensure_address_range(address, size, stage)
        with _ProcessHandle(process_id, _PROCESS_QUERY_INFORMATION | _PROCESS_VM_READ, stage) as handle:
            content = _read_with_handle(handle, process_id, address, size, stage)
        return {
            "processId": process_id,
            "address": address,
            "bytesRead": len(content),
            "contentBase64": base64.b64encode(content).decode("ascii"),
            "sha256": hashlib.sha256(content).hexdigest(),
        }

    def write(self, process_id: int, address: int, content_base64: str) -> dict[str, Any]:
        stage = "memory.write"
        _require_windows(stage)
        _validate_process_id(process_id, stage)
        if not isinstance(content_base64, str):
            raise ToolError("memory.invalid_content", "contentBase64 must be a base64 string", stage=stage, context={"type": type(content_base64).__name__})
        try:
            content = base64.b64decode(content_base64, validate=True)
        except (binascii.Error, ValueError) as error:
            raise ToolError("memory.invalid_content", "contentBase64 is not valid canonical base64", stage=stage, context={"encodedLength": len(content_base64)}) from error
        _validate_size(len(content), MAX_TRANSFER_BYTES, stage)
        _ensure_address_range(address, len(content), stage)
        access = _PROCESS_QUERY_INFORMATION | _PROCESS_VM_OPERATION | _PROCESS_VM_READ | _PROCESS_VM_WRITE
        with _ProcessHandle(process_id, access, stage) as handle:
            source = (ctypes.c_ubyte * len(content)).from_buffer_copy(content)
            transferred = ctypes.c_size_t()
            ctypes.set_last_error(0)
            success = _WriteProcessMemory(handle, ctypes.c_void_p(address), source, len(content), ctypes.byref(transferred))
            actual = int(transferred.value)
            if not success or actual != len(content):
                error = ctypes.get_last_error()
                code = "memory.partial_write" if actual else "memory.access_denied" if error == 5 else "memory.write_failed"
                raise ToolError(
                    code,
                    _windows_error_message(error, "WriteProcessMemory did not write the requested range"),
                    stage=stage,
                    context={"processId": process_id, "address": address, "requestedBytes": len(content), "bytesWritten": actual, "winError": error},
                    status="indeterminate" if actual else "failed",
                )
            verified = _read_with_handle(handle, process_id, address, len(content), "memory.write.verify")
        equal = verified == content
        requested_hash = hashlib.sha256(content).hexdigest()
        verified_hash = hashlib.sha256(verified).hexdigest()
        if not equal:
            raise ToolError(
                "memory.write_verification_failed",
                "Memory content did not match after WriteProcessMemory",
                stage="memory.write.verify",
                context={"processId": process_id, "address": address, "bytesWritten": actual, "requestedSha256": requested_hash, "verifiedSha256": verified_hash},
                status="indeterminate",
            )
        return {
            "processId": process_id,
            "address": address,
            "bytesWritten": actual,
            "requestedSha256": requested_hash,
            "verifiedSha256": verified_hash,
            "verifiedEqual": equal,
            "verifiedContentBase64": base64.b64encode(verified).decode("ascii"),
        }

    def pattern_scan(self, context: Any, process_id: int, start_address: int, size: int, pattern: str, max_matches: int = MAX_MATCHES) -> dict[str, Any]:
        stage = "memory.pattern_scan"
        _require_windows(stage)
        _validate_process_id(process_id, stage)
        _validate_size(size, MAX_SCAN_BYTES, stage)
        _validate_size(max_matches, MAX_MATCHES, stage)
        end_address = _ensure_address_range(start_address, size, stage)
        values, masks = _parse_pattern(pattern)
        matches: list[int] = []
        readable_bytes = 0
        chunk_size = MAX_TRANSFER_BYTES
        with _ProcessHandle(process_id, _PROCESS_QUERY_INFORMATION | _PROCESS_VM_READ, stage) as handle:
            regions = _query_regions(handle, start_address, size, _MAX_SCAN_REGIONS, stage, fail_on_limit=True)
            carry = b""
            previous_end: int | None = None
            for region in regions:
                context.check_cancelled(stage)
                region_start = int(region["rangeStart"])
                region_end = region_start + int(region["rangeSize"])
                if not region["readable"]:
                    carry = b""
                    previous_end = region_end
                    context.progress("scanning process memory", min(1.0, (region_end - start_address) / size))
                    continue
                if previous_end != region_start:
                    carry = b""
                address = region_start
                while address < region_end and len(matches) < max_matches:
                    context.check_cancelled(stage)
                    count = min(chunk_size, region_end - address)
                    content = _read_with_handle(handle, process_id, address, count, stage)
                    readable_bytes += len(content)
                    data = carry + content
                    data_address = address - len(carry)
                    remaining = max_matches - len(matches)
                    for offset in _matching_offsets(data, values, masks, remaining + len(carry)):
                        match_address = data_address + offset
                        if match_address + len(values) <= address and carry:
                            continue
                        if match_address < start_address or match_address + len(values) > end_address:
                            continue
                        matches.append(match_address)
                        if len(matches) >= max_matches:
                            break
                    overlap = min(len(values) - 1, len(data))
                    carry = data[-overlap:] if overlap else b""
                    address += count
                    context.progress("scanning process memory", min(1.0, (address - start_address) / size))
                previous_end = region_end
                if len(matches) >= max_matches:
                    break
        return {
            "currentState": {
                "processId": process_id,
                "startAddress": start_address,
                "size": size,
                "pattern": pattern,
                "patternBytes": len(values),
                "matches": matches,
                "matchCount": len(matches),
                "readableBytesScanned": readable_bytes,
                "truncated": len(matches) >= max_matches,
            },
            "evidence": [{"kind": "memory_pattern_matches", "source": f"pid:{process_id}", "addresses": matches, "readableBytesScanned": readable_bytes}],
        }
