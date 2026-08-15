from __future__ import annotations

from collections import deque
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time
import uuid
from typing import Any, Callable

from .model import ToolError, utc_now

DEFAULT_DAP_LEASE_SECONDS = 60.0
MAX_DAP_LEASE_SECONDS = 3600.0

class DapSession:
    def __init__(
        self,
        adapter: str,
        adapter_arguments: list[str],
        target_pid: int,
        attach_arguments: dict[str, Any],
        timeout_seconds: float,
        event_sink: Callable[[str, dict[str, Any]], None] | None = None,
        lease_seconds: float = DEFAULT_DAP_LEASE_SECONDS,
    ) -> None:
        self.session_id = f"dap-{uuid.uuid4().hex[:16]}"
        self.adapter = adapter
        self.target_pid = target_pid
        self.created_at = utc_now()
        self._timeout = timeout_seconds
        self._lease_seconds = min(max(float(lease_seconds), 0.1), MAX_DAP_LEASE_SECONDS)
        self._last_activity_monotonic = time.monotonic()
        self._last_activity_at = self.created_at
        self._sequence = 1
        self._write_lock = threading.Lock()
        self._condition = threading.Condition()
        self._responses: dict[int, dict[str, Any]] = {}
        self._configuration_lock = threading.Lock()
        self._events: deque[dict[str, Any]] = deque(maxlen=1000)
        self._stderr: deque[str] = deque(maxlen=500)
        self._closed = False
        self._stopped_thread_id: int | None = None
        self._active_requests = 0
        self._configuration_done = False
        self._event_sink = event_sink
        creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        try:
            self.process = subprocess.Popen(
                [adapter, *adapter_arguments],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                creationflags=creation_flags,
                start_new_session=os.name != "nt",
            )
        except OSError as error:
            raise ToolError(
                "debug.adapter_launch_failed",
                str(error),
                stage="debug.launch",
                context={"adapter": adapter, "arguments": adapter_arguments},
            ) from error
        self._reader = threading.Thread(target=self._read_messages, name=f"{self.session_id}-stdout", daemon=True)
        self._error_reader = threading.Thread(target=self._read_stderr, name=f"{self.session_id}-stderr", daemon=True)
        self._reader.start()
        self._error_reader.start()
        try:
            initialized = self.request(
                "initialize",
                {
                    "clientID": "ue4ss-mcp-host",
                    "clientName": "UE4SS AI MCP Host",
                    "adapterID": Path(adapter).stem,
                    "pathFormat": "path",
                    "linesStartAt1": True,
                    "columnsStartAt1": True,
                    "supportsVariableType": True,
                    "supportsVariablePaging": True,
                    "supportsRunInTerminalRequest": False,
                },
            )
            self.capabilities = initialized.get("body", {})
            pid_key = "pid" if "lldb" in Path(adapter).stem.lower() else "processId"
            attached = self.request("attach", {pid_key: target_pid, **attach_arguments}, timeout=max(timeout_seconds, 30))
            if not attached.get("success", False):
                raise self._response_error("attach", attached)
            self.wait_event("initialized", timeout=max(timeout_seconds, 30))
        except Exception:
            self.close(terminate_debuggee=False)
            raise


    def renew(self) -> None:
        with self._condition:
            self._renew_locked()

    def lease_expired(self, now: float | None = None) -> bool:
        with self._condition:
            return self._active_requests == 0 and (now if now is not None else time.monotonic()) - self._last_activity_monotonic >= self._lease_seconds

    def _renew_locked(self) -> None:
        self._last_activity_monotonic = time.monotonic()
        self._last_activity_at = utc_now()

    @property
    def lease_seconds(self) -> float:
        return self._lease_seconds

    def configuration_done(self) -> dict[str, Any]:
        with self._configuration_lock:
            supported = bool(self.capabilities.get("supportsConfigurationDoneRequest", False))
            if not supported:
                return {
                    "type": "response",
                    "command": "configurationDone",
                    "success": True,
                    "body": {"supported": False, "sent": False, "alreadyConfigured": False},
                }
            if self._configuration_done:
                return {
                    "type": "response",
                    "command": "configurationDone",
                    "success": True,
                    "body": {"supported": True, "sent": False, "alreadyConfigured": True},
                }
            response = self.request("configurationDone")
            self._configuration_done = True
            return {
                **response,
                "body": {
                    **response.get("body", {}),
                    "supported": True,
                    "sent": True,
                    "alreadyConfigured": False,
                },
            }

    def resume_before_detach(self) -> dict[str, Any]:
        with self._condition:
            thread_id = self._stopped_thread_id
        if thread_id is None:
            try:
                threads = self.request("threads", timeout=min(self._timeout, 2))
                thread_records = threads.get("body", {}).get("threads", [])
                if thread_records and isinstance(thread_records[0].get("id"), int):
                    thread_id = thread_records[0]["id"]
            except (AttributeError, ToolError):
                thread_id = None
        if thread_id is None:
            return {"attempted": False, "resumed": False, "reason": "no_thread"}
        try:
            self.request("continue", {"threadId": thread_id, "singleThread": False}, timeout=min(self._timeout, 2))
            return {"attempted": True, "resumed": True, "threadId": thread_id}
        except ToolError as error:
            return {"attempted": True, "resumed": False, "threadId": thread_id, "error": error.as_dict()}

    def request(self, command: str, arguments: dict[str, Any] | None = None, timeout: float | None = None) -> dict[str, Any]:
        with self._condition:
            self._active_requests += 1
            self._renew_locked()
        try:
            return self._request(command, arguments, timeout)
        finally:
            with self._condition:
                self._active_requests -= 1
                self._renew_locked()
                self._condition.notify_all()

    def _request(self, command: str, arguments: dict[str, Any] | None = None, timeout: float | None = None) -> dict[str, Any]:
        if self._closed or self.process.poll() is not None:
            raise ToolError("debug.session_closed", "The debug adapter session is closed", stage="debug.request", context={"sessionId": self.session_id})
        with self._write_lock:
            sequence = self._sequence
            self._sequence += 1
            message = {"seq": sequence, "type": "request", "command": command, "arguments": arguments or {}}
            payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
            assert self.process.stdin is not None
            try:
                self.process.stdin.write(f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii") + payload)
                self.process.stdin.flush()
            except OSError as error:
                raise ToolError("debug.adapter_disconnected", str(error), stage="debug.write", context={"sessionId": self.session_id}) from error
        deadline = time.monotonic() + (timeout or self._timeout)
        with self._condition:
            while sequence not in self._responses:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise ToolError(
                        "debug.request_timeout",
                        f"Debug adapter did not answer '{command}'",
                        stage="debug.request",
                        context={"sessionId": self.session_id, "command": command},
                        retryable=True,
                    )
                if self.process.poll() is not None:
                    raise ToolError(
                        "debug.adapter_exited",
                        "Debug adapter exited before responding",
                        stage="debug.request",
                        context={"sessionId": self.session_id, "command": command, "returnCode": self.process.returncode, "stderr": list(self._stderr)},
                    )
                self._condition.wait(min(remaining, 0.2))
            response = self._responses.pop(sequence)
        if not response.get("success", False):
            raise self._response_error(command, response)
        return response

    def wait_event(self, event_name: str, timeout: float | None = None) -> dict[str, Any]:
        deadline = time.monotonic() + (timeout or self._timeout)
        with self._condition:
            while True:
                for index, event in enumerate(self._events):
                    if event.get("event") == event_name:
                        del self._events[index]
                        return event
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise ToolError(
                        "debug.event_timeout",
                        f"Debug adapter did not emit '{event_name}'",
                        stage="debug.event",
                        context={"sessionId": self.session_id, "event": event_name, "recentEvents": list(self._events)[-20:]},
                        retryable=True,
                    )
                self._condition.wait(min(remaining, 0.2))

    def snapshot(self) -> dict[str, Any]:
        with self._condition:
            last_activity_at = self._last_activity_at
            remaining = max(0.0, self._lease_seconds - (time.monotonic() - self._last_activity_monotonic))
        return {
            "sessionId": self.session_id,
            "adapter": self.adapter,
            "targetProcessId": self.target_pid,
            "createdAt": self.created_at,
            "connected": not self._closed and self.process.poll() is None,
            "capabilities": self.capabilities,
            "leaseSeconds": self._lease_seconds,
            "lastActivityAt": last_activity_at,
            "leaseRemainingSeconds": round(remaining, 3),
            "renewalPolicy": "eachSessionOperation",
            "recentEvents": list(self._events)[-100:],
            "recentStderr": list(self._stderr)[-100:],
        }

    def close(self, *, terminate_debuggee: bool) -> None:
        if self._closed:
            return
        try:
            if self.process.poll() is None:
                try:
                    self.request("disconnect", {"restart": False, "terminateDebuggee": terminate_debuggee, "suspendDebuggee": False}, timeout=5)
                except ToolError:
                    pass
        finally:
            self._closed = True
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=5)
            for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
                if stream:
                    stream.close()
            self._reader.join(timeout=5)
            self._error_reader.join(timeout=5)
            with self._condition:
                self._condition.notify_all()

    def _read_messages(self) -> None:
        assert self.process.stdout is not None
        stream = self.process.stdout
        try:
            while not self._closed:
                headers: dict[str, str] = {}
                while True:
                    line = stream.readline()
                    if not line:
                        return
                    if line in {b"\r\n", b"\n"}:
                        break
                    text = line.decode("ascii", errors="replace").strip()
                    if ":" in text:
                        name, value = text.split(":", 1)
                        headers[name.casefold()] = value.strip()
                length = int(headers.get("content-length", "0"))
                if length <= 0 or length > 16 * 1024 * 1024:
                    return
                payload = stream.read(length)
                if len(payload) != length:
                    return
                message = json.loads(payload)
                event_to_publish: dict[str, Any] | None = None
                with self._condition:
                    if message.get("type") == "response" and isinstance(message.get("request_seq"), int):
                        self._responses[message["request_seq"]] = message
                    elif message.get("type") == "event":
                        self._events.append(message)
                        event_to_publish = message
                        if message.get("event") == "stopped":
                            thread_id = message.get("body", {}).get("threadId")
                            self._stopped_thread_id = thread_id if isinstance(thread_id, int) else None
                        elif message.get("event") in {"continued", "terminated", "exited"}:
                            self._stopped_thread_id = None
                    elif message.get("type") == "request":
                        self._events.append({"type": "event", "event": "unsupportedAdapterRequest", "body": message})
                    self._condition.notify_all()
                if event_to_publish is not None and self._event_sink:
                    try:
                        self._event_sink(self.session_id, event_to_publish)
                    except Exception:
                        pass
        except (OSError, ValueError, json.JSONDecodeError):
            return
        finally:
            with self._condition:
                self._condition.notify_all()

    def _read_stderr(self) -> None:
        assert self.process.stderr is not None
        for line in iter(self.process.stderr.readline, b""):
            self._stderr.append(line.decode("utf-8", errors="replace").rstrip("\r\n"))

    def _response_error(self, command: str, response: dict[str, Any]) -> ToolError:
        return ToolError(
            "debug.adapter_error",
            str(response.get("message") or f"Debug adapter rejected '{command}'"),
            stage=f"debug.{command}",
            context={"sessionId": self.session_id, "response": response, "stderr": list(self._stderr)[-100:]},
        )


class DebugSessionManager:
    def __init__(
        self,
        adapter_resolver: Callable[[], str | None],
        event_sink: Callable[[str, dict[str, Any]], None] | None = None,
        *,
        default_lease_seconds: float = DEFAULT_DAP_LEASE_SECONDS,
        watchdog_interval_seconds: float = 1.0,
    ) -> None:
        self._adapter_resolver = adapter_resolver
        self._event_sink = event_sink
        self._default_lease_seconds = min(max(float(default_lease_seconds), 0.1), MAX_DAP_LEASE_SECONDS)
        self._watchdog_interval_seconds = max(float(watchdog_interval_seconds), 0.01)
        self._sessions: dict[str, DapSession] = {}
        self._lock = threading.RLock()
        self._watchdog_stop = threading.Event()
        self._watchdog = threading.Thread(target=self._watch_expired_sessions, name="dap-lease-watchdog", daemon=True)
        self._watchdog.start()

    def profile(self) -> dict[str, Any]:
        adapter = self._adapter_resolver()
        return {
            "available": adapter is not None,
            "adapter": adapter,
            "supportedOperations": ["attach", "setBreakpoints", "threads", "stackTrace", "scopes", "variables", "evaluate", "pause", "continue", "next", "stepIn", "stepOut", "disconnect"] if adapter else [],
            "missingConditions": [] if adapter else ["Configure a DAP executable such as lldb-dap as tools.debugger"],
            "leasePolicy": {
                "defaultSeconds": self._default_lease_seconds,
                "maximumSeconds": MAX_DAP_LEASE_SECONDS,
                "renewal": "eachSessionOperation",
                "expiryAction": "continueThenDetach",
            },
        }

    def attach(
        self,
        process_id: int,
        adapter_arguments: list[str],
        attach_arguments: dict[str, Any],
        timeout_seconds: float,
        lease_seconds: float | None = None,
    ) -> dict[str, Any]:
        adapter = self._adapter_resolver()
        if not adapter:
            raise ToolError(
                "debug.adapter_unavailable",
                "No DAP-compatible native debugger is configured",
                stage="debug.profile",
                suggested_actions=["Configure tools.debugger with a DAP executable such as lldb-dap"],
            )
        effective_lease = self._default_lease_seconds if lease_seconds is None else float(lease_seconds)
        if effective_lease <= 0 or effective_lease > MAX_DAP_LEASE_SECONDS:
            raise ToolError(
                "debug.invalid_lease",
                f"Debug session lease must be greater than zero and no more than {MAX_DAP_LEASE_SECONDS:g} seconds",
                stage="debug.attach",
                context={"leaseSeconds": effective_lease},
            )
        session = DapSession(
            adapter,
            adapter_arguments,
            process_id,
            attach_arguments,
            timeout_seconds,
            self._event_sink,
            effective_lease,
        )
        session.renew()
        with self._lock:
            self._sessions[session.session_id] = session
        return session.snapshot()

    def get(self, session_id: str) -> DapSession:
        with self._lock:
            session = self._sessions.get(session_id)
            if session:
                session.renew()
        if not session:
            raise ToolError("debug.session_not_found", f"Debug session '{session_id}' was not found", stage="debug.lookup", context={"sessionId": session_id})
        return session

    def list(self) -> list[dict[str, Any]]:
        with self._lock:
            return [session.snapshot() for session in self._sessions.values()]

    def detach(self, session_id: str, *, terminate_debuggee: bool = False) -> dict[str, Any]:
        with self._lock:
            session = self._sessions.pop(session_id, None)
            if session:
                session.renew()
        if not session:
            raise ToolError("debug.session_not_found", f"Debug session '{session_id}' was not found", stage="debug.lookup", context={"sessionId": session_id})
        session.close(terminate_debuggee=terminate_debuggee)
        return {"sessionId": session_id, "connected": False, "targetProcessTerminated": terminate_debuggee}

    def close(self) -> None:
        self._watchdog_stop.set()
        self._watchdog.join()
        with self._lock:
            sessions = list(self._sessions.values())
            self._sessions.clear()
        for session in sessions:
            session.close(terminate_debuggee=False)

    def _watch_expired_sessions(self) -> None:
        while not self._watchdog_stop.wait(self._watchdog_interval_seconds):
            now = time.monotonic()
            with self._lock:
                expired = [
                    (session_id, session)
                    for session_id, session in self._sessions.items()
                    if session.lease_expired(now)
                ]
                for session_id, _ in expired:
                    self._sessions.pop(session_id, None)
            for session_id, session in expired:
                resume = session.resume_before_detach()
                if self._event_sink:
                    try:
                        self._event_sink(
                            session_id,
                            {
                                "type": "event",
                                "event": "leaseExpired",
                                "body": {"leaseSeconds": session.lease_seconds, "resume": resume},
                            },
                        )
                    except Exception:
                        pass
                session.close(terminate_debuggee=False)
