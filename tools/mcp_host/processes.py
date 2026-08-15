from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time
from typing import Any, Iterator

from .model import ToolError, utc_now
from .workspace import Workspace


MAX_ARGUMENTS = 256
MAX_ARGUMENT_BYTES = 32 * 1024
MAX_DISCOVERY_RESULTS = 1000
MAX_DISCOVERY_CANDIDATES = 10000
MAX_TERMINATION_SECONDS = 60.0
MAX_PATH_INPUT_BYTES = 32 * 1024
MAX_FILTER_INPUT_BYTES = 1024
MAX_TRACKED_PROCESSES = 4096
MAX_RETIRED_PROCESS_IDS = 4096


@dataclass(frozen=True, slots=True)
class LaunchSpec:
    root: str
    executable: str
    arguments: tuple[str, ...]
    working_directory: str


@dataclass(slots=True)
class StartedProcess:
    process: subprocess.Popen[bytes]
    spec: LaunchSpec
    started_at: str


@dataclass(frozen=True, slots=True)
class ProcessSnapshot:
    pid: int
    executable_name: str
    executable_path: Path | None


class ProcessManager:
    """Bounded process discovery and lifecycle control for explicitly scoped games."""

    def __init__(self, workspace: Workspace) -> None:
        self.workspace = workspace
        self._started: dict[int, StartedProcess] = {}
        self._retired: dict[int, int | None] = {}
        self._lock = threading.RLock()

    def profile(self) -> dict[str, Any]:
        self._reap()
        backend = self._backend()
        available = backend is not None
        return {
            "available": available,
            "status": "available" if available else "unavailable",
            "platform": sys.platform,
            "backend": backend,
            "supportedOperations": ["discover", "start", "stop", "restart"] if available else [],
            "missingConditions": [] if available else ["Process discovery requires Windows or a Linux /proc filesystem"],
            "roots": self.workspace.root_snapshot(),
            "limits": {
                "maxArguments": MAX_ARGUMENTS,
                "maxArgumentBytes": MAX_ARGUMENT_BYTES,
                "maxDiscoveryResults": MAX_DISCOVERY_RESULTS,
                "maxDiscoveryCandidates": MAX_DISCOVERY_CANDIDATES,
                "maxTerminationSeconds": MAX_TERMINATION_SECONDS,
                "maxTrackedProcesses": MAX_TRACKED_PROCESSES,
                "maxRetiredProcessIds": MAX_RETIRED_PROCESS_IDS,
            },
            "evidence": [{"kind": "platform_probe", "source": sys.platform, "backend": backend}],
        }

    def discover(
        self,
        *,
        root: str | None = None,
        executable_name: str | None = None,
        cursor: int = 0,
        limit: int = 200,
    ) -> dict[str, Any]:
        self._require_backend("process.discover")
        self._reap()
        self._validate_optional_text(root, "root", MAX_FILTER_INPUT_BYTES)
        self._validate_optional_text(executable_name, "executableName", MAX_FILTER_INPUT_BYTES)
        selected_root = self.workspace.resolve(root).root if root is not None else None
        cursor = max(cursor, 0)
        limit = min(max(limit, 1), MAX_DISCOVERY_RESULTS)
        name_filter = executable_name.casefold() if executable_name is not None else None
        candidates: list[dict[str, Any]] = []
        scanned = 0
        for snapshot in self._iter_processes():
            scanned += 1
            if snapshot.executable_path is None:
                continue
            matches = self._root_matches(snapshot.executable_path)
            if selected_root is not None and not any(Path(item["path"]) == selected_root for item in matches):
                continue
            if name_filter is not None and snapshot.executable_name.casefold() != name_filter:
                continue
            with self._lock:
                started = self._started.get(snapshot.pid)
                exit_code = started.process.poll() if started is not None else None
                host_started = started is not None and exit_code is None
            candidate_evidence = ["running_process"]
            candidate_evidence.extend(f"root:{item['name']}" for item in matches)
            if name_filter is not None:
                candidate_evidence.append(f"executable:{snapshot.executable_name}")
            candidates.append(
                {
                    "pid": snapshot.pid,
                    "executableName": snapshot.executable_name,
                    "executablePath": snapshot.executable_path.as_posix(),
                    "rootMatches": matches,
                    "candidate": True,
                    "running": True,
                    "hostStarted": host_started,
                    "exitCode": exit_code,
                    "candidateEvidence": candidate_evidence,
                }
            )
            if len(candidates) >= MAX_DISCOVERY_CANDIDATES:
                break
        candidates.sort(key=lambda item: (str(item["executableName"]).casefold(), int(item["pid"])))
        page = candidates[cursor : cursor + limit]
        next_cursor = cursor + len(page)
        truncated = len(candidates) >= MAX_DISCOVERY_CANDIDATES
        return {
            "platform": sys.platform,
            "filters": {"root": root, "executableName": executable_name},
            "processes": page,
            "returned": len(page),
            "totalCandidates": len(candidates),
            "nextCursor": next_cursor,
            "endOfList": not truncated and next_cursor >= len(candidates),
            "truncated": truncated,
            "scanned": scanned,
            "evidence": [
                {
                    "kind": "process_snapshot",
                    "source": self._backend(),
                    "scanned": scanned,
                    "matched": len(candidates),
                    "boundedAt": MAX_DISCOVERY_CANDIDATES,
                }
            ],
        }

    def start(
        self,
        *,
        root: str,
        executable: str,
        arguments: list[str],
        working_directory: str | None = None,
    ) -> dict[str, Any]:
        self._require_backend("process.start")
        self._validate_required_text(root, "root", MAX_FILTER_INPUT_BYTES)
        self._validate_required_text(executable, "executable", MAX_PATH_INPUT_BYTES)
        self._validate_optional_text(working_directory, "workingDirectory", MAX_PATH_INPUT_BYTES)
        self._validate_arguments(arguments)
        executable_path = self.workspace.resolve(root, executable, must_exist=True)
        if not executable_path.path.is_file():
            raise ToolError(
                "process.executable_not_file",
                "The requested executable is not a file",
                stage="process.start",
                context={"root": root, "executable": executable_path.relative},
                status="rejected",
            )
        directory = self.workspace.resolve(root, working_directory or "", must_exist=True) if working_directory is not None else None
        cwd = directory.path if directory is not None else executable_path.path.parent
        if not cwd.is_dir():
            raise ToolError(
                "process.working_directory_not_directory",
                "The requested working directory is not a directory",
                stage="process.start",
                context={"root": root, "workingDirectory": working_directory},
                status="rejected",
            )
        cwd_relative = cwd.relative_to(executable_path.root).as_posix()
        creationflags = subprocess.CREATE_NEW_PROCESS_GROUP if sys.platform == "win32" else 0
        command = [str(executable_path.path), *arguments]
        self._reap()
        with self._lock:
            if len(self._started) >= MAX_TRACKED_PROCESSES:
                raise ToolError(
                    "process.tracking_capacity",
                    "The bounded host-started process tracking capacity is full",
                    stage="process.start",
                    context={"maximum": MAX_TRACKED_PROCESSES},
                    retryable=True,
                )
            try:
                process = subprocess.Popen(
                    command,
                    cwd=cwd,
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    shell=False,
                    creationflags=creationflags,
                )
            except (OSError, ValueError) as error:
                raise ToolError(
                    "process.start_failed",
                    str(error),
                    stage="process.start",
                    context={
                        "root": root,
                        "executable": executable_path.relative,
                        "workingDirectory": cwd_relative,
                        "winerror": getattr(error, "winerror", None),
                        "errno": getattr(error, "errno", None),
                    },
                ) from error
            record = StartedProcess(
                process=process,
                spec=LaunchSpec(root, executable_path.relative, tuple(arguments), cwd_relative),
                started_at=utc_now(),
            )
            self._retired.pop(process.pid, None)
            self._started[process.pid] = record
        state = self._started_state(process.pid, record)
        return {
            "currentState": state,
            "actualChanges": [{"kind": "process", "action": "started", "pid": process.pid, "executablePath": state["executablePath"]}],
            "evidence": [
                {
                    "kind": "process_launch",
                    "source": state["executablePath"],
                    "pid": process.pid,
                    "arguments": list(arguments),
                    "workingDirectory": cwd.as_posix(),
                    "shell": False,
                }
            ],
        }

    def stop(self, *, pid: int, force: bool = False, timeout_seconds: float = 5.0) -> dict[str, Any]:
        self._require_backend("process.stop")
        timeout_seconds = min(max(timeout_seconds, 0.1), MAX_TERMINATION_SECONDS)
        if pid == os.getpid():
            raise ToolError(
                "process.self_target_forbidden",
                "The MCP host cannot stop itself",
                stage="process.stop",
                context={"pid": pid},
                status="rejected",
            )
        self._reap()
        with self._lock:
            record = self._started.get(pid)
            retired_exit_code = self._retired.get(pid)
            was_retired = pid in self._retired
        if was_retired:
            raise ToolError(
                "process.not_running",
                f"Host-started process {pid} has already exited",
                stage="process.lookup",
                context={"pid": pid, "exitCode": retired_exit_code, "hostStarted": True},
            )
        snapshot = self._lookup_process(pid) if record is None else None
        if record is None and snapshot is None:
            raise ToolError(
                "process.not_found",
                f"Process {pid} is not running",
                stage="process.lookup",
                context={"pid": pid},
            )
        before = self._state_for_target(pid, record, snapshot)
        graceful_method = self._request_graceful_stop(pid, record)
        stopped = self._wait_for_exit(pid, record, timeout_seconds)
        forced = False
        if not stopped and force:
            self._force_stop(pid, record)
            forced = True
            stopped = self._wait_for_exit(pid, record, timeout_seconds)
        after = dict(before)
        after["running"] = not stopped
        after["exitCode"] = record.process.poll() if record is not None else None
        after["gracefulMethod"] = graceful_method
        after["forced"] = forced
        if stopped and record is not None:
            self._retire(pid, record)
        status = "succeeded" if stopped else "partial"
        action = "graceful_termination_unavailable" if graceful_method == "none_available" else "graceful_termination_requested"
        actions = [{"kind": "process", "action": action, "pid": pid, "method": graceful_method}]
        if forced:
            actions.append({"kind": "process", "action": "forced_termination_requested", "pid": pid})
        return {
            "status": status,
            "currentState": after,
            "actualChanges": actions,
            "evidence": [
                {
                    "kind": "process_exit_probe",
                    "source": str(pid),
                    "runningBefore": True,
                    "runningAfter": not stopped,
                    "timeoutSeconds": timeout_seconds,
                    "force": force,
                }
            ],
        }

    def restart(self, *, pid: int, force: bool = False, timeout_seconds: float = 5.0) -> dict[str, Any]:
        self._require_backend("process.restart")
        self._reap()
        with self._lock:
            record = self._started.get(pid)
        if record is None or record.process.poll() is not None:
            raise ToolError(
                "process.restart_unavailable",
                "Restart requires a running process started by this host",
                stage="process.restart",
                context={"pid": pid, "hostStarted": record is not None},
            )
        spec = record.spec
        stopped = self.stop(pid=pid, force=force, timeout_seconds=timeout_seconds)
        if stopped.get("status") != "succeeded":
            error = ToolError(
                "process.termination_timeout",
                "The process did not exit within the bounded termination timeout",
                stage="process.restart",
                context={"pid": pid, "timeoutSeconds": timeout_seconds, "force": force},
                retryable=True,
                suggested_actions=["Retry with force=true only when forced termination is acceptable"],
            )
            return {
                "status": "partial",
                "currentState": {
                    "previousProcess": stopped["currentState"],
                    "process": None,
                    "restartError": error.as_dict(),
                },
                "actualChanges": list(stopped.get("actualChanges", [])),
                "evidence": list(stopped.get("evidence", [])),
                "errors": [error.as_dict()],
                "suggestedActions": list(error.suggested_actions),
            }
        try:
            started = self.start(
                root=spec.root,
                executable=spec.executable,
                arguments=list(spec.arguments),
                working_directory=spec.working_directory,
            )
        except ToolError as error:
            return {
                "status": "partial",
                "currentState": {
                    "previousProcess": stopped["currentState"],
                    "process": None,
                    "restartError": error.as_dict(),
                },
                "actualChanges": list(stopped.get("actualChanges", [])),
                "evidence": list(stopped.get("evidence", [])),
                "errors": [error.as_dict()],
                "suggestedActions": list(error.suggested_actions),
            }
        return {
            "currentState": {"previousProcess": stopped["currentState"], "process": started["currentState"]},
            "actualChanges": [*stopped.get("actualChanges", []), *started.get("actualChanges", [])],
            "evidence": [*stopped.get("evidence", []), *started.get("evidence", [])],
        }

    def _backend(self) -> str | None:
        if sys.platform == "win32":
            return "windows-toolhelp32"
        if sys.platform.startswith("linux") and Path("/proc").is_dir():
            return "linux-procfs"
        return None

    def _require_backend(self, stage: str) -> None:
        if self._backend() is None:
            raise ToolError(
                "capability.unavailable",
                "Process discovery and control are unavailable on this platform",
                stage=stage,
                context={"platform": sys.platform, "supported": ["win32", "linux-procfs"]},
            )

    @staticmethod
    def _validate_required_text(value: str, argument: str, maximum_bytes: int) -> None:
        if not isinstance(value, str) or not value:
            raise ToolError(
                "transport.invalid_arguments",
                f"Argument '{argument}' must be a non-empty string",
                stage="transport.arguments",
                context={"argument": argument},
            )
        ProcessManager._validate_text_size(value, argument, maximum_bytes)

    @staticmethod
    def _validate_optional_text(value: str | None, argument: str, maximum_bytes: int) -> None:
        if value is None:
            return
        ProcessManager._validate_required_text(value, argument, maximum_bytes)

    @staticmethod
    def _validate_text_size(value: str, argument: str, maximum_bytes: int) -> None:
        size = len(value.encode("utf-8"))
        if size > maximum_bytes:
            raise ToolError(
                "transport.invalid_arguments",
                f"Argument '{argument}' exceeds its bounded size",
                stage="transport.arguments",
                context={"argument": argument, "bytes": size, "maximumBytes": maximum_bytes},
            )

    @staticmethod
    def _validate_arguments(arguments: list[str]) -> None:
        if len(arguments) > MAX_ARGUMENTS:
            raise ToolError(
                "process.arguments_too_large",
                "Too many process arguments",
                stage="process.start",
                context={"count": len(arguments), "maximum": MAX_ARGUMENTS},
                status="rejected",
            )
        byte_count = 0
        for index, argument in enumerate(arguments):
            if not isinstance(argument, str):
                raise ToolError(
                    "transport.invalid_arguments",
                    "Process arguments must be strings",
                    stage="transport.arguments",
                    context={"argument": "arguments", "index": index},
                )
            byte_count += len(argument.encode("utf-8"))
        if byte_count > MAX_ARGUMENT_BYTES:
            raise ToolError(
                "process.arguments_too_large",
                "The encoded process arguments exceed the bounded limit",
                stage="process.start",
                context={"bytes": byte_count, "maximum": MAX_ARGUMENT_BYTES},
                status="rejected",
            )

    def _root_matches(self, executable_path: Path | None) -> list[dict[str, str]]:
        if executable_path is None:
            return []
        resolved = executable_path.resolve(strict=False)
        matches: list[dict[str, str]] = []
        for name, root in sorted(self.workspace.roots.items()):
            try:
                relative = resolved.relative_to(root)
            except ValueError:
                continue
            matches.append({"name": name, "path": root.as_posix(), "relativeExecutable": relative.as_posix()})
        return matches

    def _iter_processes(self) -> Iterator[ProcessSnapshot]:
        if sys.platform == "win32":
            yield from self._windows_processes()
            return
        yield from self._linux_processes()

    def _lookup_process(self, pid: int) -> ProcessSnapshot | None:
        for snapshot in self._iter_processes():
            if snapshot.pid == pid:
                return snapshot
        return None

    def _reap(self) -> None:
        with self._lock:
            exited = [(pid, record) for pid, record in self._started.items() if record.process.poll() is not None]
        for pid, record in exited:
            self._retire(pid, record)

    def _retire(self, pid: int, record: StartedProcess) -> None:
        with self._lock:
            if self._started.get(pid) is not record:
                return
            self._started.pop(pid)
            self._retired.pop(pid, None)
            self._retired[pid] = record.process.poll()
            while len(self._retired) > MAX_RETIRED_PROCESS_IDS:
                self._retired.pop(next(iter(self._retired)))

    def _started_state(self, pid: int, record: StartedProcess) -> dict[str, Any]:
        executable = self.workspace.resolve(record.spec.root, record.spec.executable).path
        exit_code = record.process.poll()
        return {
            "pid": pid,
            "executablePath": executable.as_posix(),
            "executableName": executable.name,
            "root": record.spec.root,
            "rootMatch": {
                "name": record.spec.root,
                "path": self.workspace.roots[record.spec.root].as_posix(),
                "relativeExecutable": record.spec.executable,
            },
            "arguments": list(record.spec.arguments),
            "workingDirectory": self.workspace.resolve(record.spec.root, record.spec.working_directory).path.as_posix(),
            "hostStarted": True,
            "startedAt": record.started_at,
            "running": exit_code is None,
            "exitCode": exit_code,
        }

    def _state_for_target(self, pid: int, record: StartedProcess | None, snapshot: ProcessSnapshot | None) -> dict[str, Any]:
        if record is not None:
            return self._started_state(pid, record)
        assert snapshot is not None
        return {
            "pid": pid,
            "executablePath": snapshot.executable_path.as_posix() if snapshot.executable_path else None,
            "executableName": snapshot.executable_name,
            "rootMatches": self._root_matches(snapshot.executable_path),
            "hostStarted": False,
            "running": True,
            "exitCode": None,
        }

    def _request_graceful_stop(self, pid: int, record: StartedProcess | None) -> str:
        if sys.platform != "win32":
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                return "already_exited"
            except OSError as error:
                raise ToolError(
                    "process.termination_failed",
                    str(error),
                    stage="process.stop.graceful",
                    context={"pid": pid, "errno": error.errno},
                ) from error
            return "sigterm"
        if record is not None:
            try:
                record.process.send_signal(signal.CTRL_BREAK_EVENT)
                return "ctrl_break"
            except (OSError, ValueError):
                pass
        closed_windows = self._windows_request_close(pid)
        return "wm_close" if closed_windows else "none_available"

    def _force_stop(self, pid: int, record: StartedProcess | None) -> None:
        if record is not None:
            try:
                record.process.kill()
                return
            except OSError as error:
                if record.process.poll() is not None:
                    return
                raise ToolError(
                    "process.termination_failed",
                    str(error),
                    stage="process.stop.force",
                    context={"pid": pid, "winerror": getattr(error, "winerror", None), "errno": error.errno},
                ) from error
        if sys.platform != "win32":
            try:
                os.kill(pid, signal.SIGKILL)
                return
            except ProcessLookupError:
                return
            except OSError as error:
                raise ToolError(
                    "process.termination_failed",
                    str(error),
                    stage="process.stop.force",
                    context={"pid": pid, "errno": error.errno},
                ) from error
        self._windows_force_stop(pid)

    def _wait_for_exit(self, pid: int, record: StartedProcess | None, timeout_seconds: float) -> bool:
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            if record is not None:
                if record.process.poll() is not None:
                    return True
            elif self._lookup_process(pid) is None:
                return True
            time.sleep(0.05)
        if record is not None:
            return record.process.poll() is not None
        return self._lookup_process(pid) is None

    @staticmethod
    def _linux_processes() -> Iterator[ProcessSnapshot]:
        proc = Path("/proc")
        try:
            entries = sorted((item for item in proc.iterdir() if item.name.isdigit()), key=lambda item: int(item.name))
        except OSError as error:
            raise ToolError(
                "capability.unavailable",
                "Linux procfs cannot be enumerated",
                stage="process.discover",
                context={"path": proc.as_posix(), "errno": error.errno},
            ) from error
        for entry in entries:
            pid = int(entry.name)
            try:
                executable = (entry / "exe").resolve(strict=True)
            except (OSError, RuntimeError):
                executable = None
            try:
                name = (entry / "comm").read_text(encoding="utf-8", errors="replace").strip()
            except OSError:
                if executable is None:
                    continue
                name = executable.name
            yield ProcessSnapshot(pid, executable.name if executable is not None else name, executable)

    @staticmethod
    def _windows_processes() -> Iterator[ProcessSnapshot]:
        import ctypes
        from ctypes import wintypes

        class PROCESSENTRY32W(ctypes.Structure):
            _fields_ = [
                ("dwSize", wintypes.DWORD),
                ("cntUsage", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t),
                ("th32ModuleID", wintypes.DWORD),
                ("cntThreads", wintypes.DWORD),
                ("th32ParentProcessID", wintypes.DWORD),
                ("pcPriClassBase", wintypes.LONG),
                ("dwFlags", wintypes.DWORD),
                ("szExeFile", wintypes.WCHAR * 260),
            ]

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        create_snapshot = kernel32.CreateToolhelp32Snapshot
        create_snapshot.argtypes = (wintypes.DWORD, wintypes.DWORD)
        create_snapshot.restype = wintypes.HANDLE
        process_first = kernel32.Process32FirstW
        process_first.argtypes = (wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W))
        process_first.restype = wintypes.BOOL
        process_next = kernel32.Process32NextW
        process_next.argtypes = (wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W))
        process_next.restype = wintypes.BOOL
        open_process = kernel32.OpenProcess
        open_process.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        open_process.restype = wintypes.HANDLE
        query_path = kernel32.QueryFullProcessImageNameW
        query_path.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD))
        query_path.restype = wintypes.BOOL
        close_handle = kernel32.CloseHandle
        close_handle.argtypes = (wintypes.HANDLE,)
        close_handle.restype = wintypes.BOOL

        snapshot_handle = create_snapshot(0x00000002, 0)
        if snapshot_handle == ctypes.c_void_p(-1).value:
            error = ctypes.get_last_error()
            raise ToolError(
                "capability.unavailable",
                "Windows process snapshot could not be created",
                stage="process.discover",
                context={"winerror": error},
            )
        try:
            entry = PROCESSENTRY32W()
            entry.dwSize = ctypes.sizeof(entry)
            more = bool(process_first(snapshot_handle, ctypes.byref(entry)))
            while more:
                pid = int(entry.th32ProcessID)
                name = str(entry.szExeFile)
                executable: Path | None = None
                process_handle = open_process(0x1000, False, pid)
                if process_handle:
                    try:
                        buffer = ctypes.create_unicode_buffer(32768)
                        size = wintypes.DWORD(len(buffer))
                        if query_path(process_handle, 0, buffer, ctypes.byref(size)):
                            executable = Path(buffer.value).resolve(strict=False)
                            name = executable.name
                    finally:
                        close_handle(process_handle)
                yield ProcessSnapshot(pid, name, executable)
                more = bool(process_next(snapshot_handle, ctypes.byref(entry)))
        finally:
            close_handle(snapshot_handle)

    @staticmethod
    def _windows_request_close(pid: int) -> int:
        import ctypes
        from ctypes import wintypes

        user32 = ctypes.WinDLL("user32", use_last_error=True)
        enum_windows = user32.EnumWindows
        get_pid = user32.GetWindowThreadProcessId
        post_message = user32.PostMessageW
        callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        enum_windows.argtypes = (callback_type, wintypes.LPARAM)
        enum_windows.restype = wintypes.BOOL
        get_pid.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
        get_pid.restype = wintypes.DWORD
        post_message.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
        post_message.restype = wintypes.BOOL
        closed = 0

        @callback_type
        def callback(window: int, _parameter: int) -> bool:
            nonlocal closed
            owner_pid = wintypes.DWORD()
            get_pid(window, ctypes.byref(owner_pid))
            if owner_pid.value == pid and post_message(window, 0x0010, 0, 0):
                closed += 1
            return True

        enum_windows(callback, 0)
        return closed

    @staticmethod
    def _windows_force_stop(pid: int) -> None:
        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        open_process = kernel32.OpenProcess
        open_process.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        open_process.restype = wintypes.HANDLE
        terminate_process = kernel32.TerminateProcess
        terminate_process.argtypes = (wintypes.HANDLE, wintypes.UINT)
        terminate_process.restype = wintypes.BOOL
        close_handle = kernel32.CloseHandle
        close_handle.argtypes = (wintypes.HANDLE,)
        close_handle.restype = wintypes.BOOL
        handle = open_process(0x0001, False, pid)
        if not handle:
            error = ctypes.get_last_error()
            if error == 87:
                return
            raise ToolError(
                "process.termination_failed",
                "Windows could not open the process for termination",
                stage="process.stop.force",
                context={"pid": pid, "winerror": error},
            )
        try:
            if not terminate_process(handle, 1):
                error = ctypes.get_last_error()
                raise ToolError(
                    "process.termination_failed",
                    "Windows could not terminate the process",
                    stage="process.stop.force",
                    context={"pid": pid, "winerror": error},
                )
        finally:
            close_handle(handle)
