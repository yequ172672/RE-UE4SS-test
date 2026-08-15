from __future__ import annotations

from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import queue
import socket
import threading
import time
import uuid
from typing import Any, Callable
from urllib.parse import parse_qs, urlsplit

from .definitions import host_tool_definitions
from .debugger import DebugSessionManager
from .events import EventLog
from .memory import MemoryManager
from .model import OperationStore, ToolError, content_result
from .mods import ModManager
from .processes import ProcessManager
from .runtime import PROTOCOL_VERSION, RuntimeClient
from .scenarios import ScenarioRunner
from .settings import SettingsManager
from .tasks import TaskRegistry
from .toolchains import Toolchains
from .workspace import Workspace


MAX_REQUEST_BYTES = 4 * 1024 * 1024
RUNTIME_SCOPED_HOST_TOOLS = frozenset({"ue4ss_tasks_list", "ue4ss_task_get", "ue4ss_task_cancel", "ue4ss_events_read"})


class McpHostServer:
    def __init__(
        self,
        *,
        roots: dict[str, str | Path],
        state_directory: str | Path,
        runtime_endpoint: str | None = None,
        tools: dict[str, str] | None = None,
        bind_address: str = "127.0.0.1",
        port: int = 0,
        max_workers: int = 4,
    ) -> None:
        if bind_address not in {"127.0.0.1", "localhost"}:
            raise ValueError("MCP host only supports an IPv4 loopback bind address")
        self.bind_address = "127.0.0.1" if bind_address == "localhost" else bind_address
        self.requested_port = port
        self.session_id = f"host-{os.getpid()}-{uuid.uuid4().hex[:16]}"
        self.events = EventLog()
        self.operations = OperationStore(self.session_id, self.events.publish)
        self.tasks = TaskRegistry(self.operations, max_workers=max_workers)
        self.workspace = Workspace(roots, state_directory)
        self.runtime = RuntimeClient(runtime_endpoint)
        self.processes = ProcessManager(self.workspace)
        self.memory = MemoryManager()
        self.toolchains = Toolchains(self.workspace, tools)
        self.debugger = DebugSessionManager(
            lambda: self.toolchains.executable("debugger", ["lldb-dap", "lldb-vscode"]),
            self._handle_dap_event,
        )
        self.mods = ModManager(self.workspace)
        self.settings = SettingsManager(self.workspace)
        self.scenarios = ScenarioRunner(self.workspace, self.runtime)
        self._httpd: ThreadingHTTPServer | None = None
        self._thread: threading.Thread | None = None
        self._host_tools = {definition["name"]: definition for definition in host_tool_definitions()}
        self._runtime_tool_cache: dict[str, dict[str, Any]] = {}
        self._started_at = time.time()

    def _handle_dap_event(self, session_id: str, event: dict[str, Any]) -> None:
        body = event.get("body", {})
        serialized_body = json.dumps(body, ensure_ascii=False, separators=(",", ":"))
        body_truncated = len(serialized_body.encode("utf-8")) > 65536
        bounded_body: Any = body if not body_truncated else {"preview": serialized_body[:65536], "truncated": True}
        snapshot = {
            "sessionId": session_id,
            "event": str(event.get("event") or ""),
            "body": bounded_body,
            "bodyTruncated": body_truncated,
        }
        self.events.publish("debug.dap_event", snapshot)
        if snapshot["event"] == "output":
            self.operations.result(
                "ue4ss_native_debug_output",
                state=snapshot,
                evidence=[{"kind": "dap_output", "source": session_id}],
            )
        if snapshot["event"] == "leaseExpired":
            self.events.publish("debug.lease_expired", snapshot)
            resume = bounded_body.get("resume", {}) if isinstance(bounded_body, dict) else {}
            if resume.get("resumed") is True:
                self.events.publish("debug.target_resumed", snapshot)
            self.operations.result(
                "ue4ss_native_debug_lease_expired",
                state=snapshot,
                evidence=[{"kind": "dap_lease_expiry", "source": session_id, "resume": resume}],
            )

    @property
    def port(self) -> int:
        return int(self._httpd.server_address[1]) if self._httpd else self.requested_port

    @property
    def endpoint(self) -> str:
        return f"http://{self.bind_address}:{self.port}/mcp"

    def start(self) -> None:
        if self._httpd:
            return
        handler = self._make_handler()
        self._httpd = ThreadingHTTPServer((self.bind_address, self.requested_port), handler)
        self._httpd.daemon_threads = True
        self._thread = threading.Thread(target=self._httpd.serve_forever, name="ue4ss-mcp-host", daemon=True)
        self._thread.start()
        self.events.publish("host.started", self.status_snapshot())

    def serve_forever(self) -> None:
        if self._httpd:
            raise RuntimeError("Server has already started")
        handler = self._make_handler()
        self._httpd = ThreadingHTTPServer((self.bind_address, self.requested_port), handler)
        self._httpd.daemon_threads = True
        self._thread = threading.current_thread()
        self.events.publish("host.started", self.status_snapshot())
        try:
            self._httpd.serve_forever()
        finally:
            self.close()

    def close(self) -> None:
        httpd = self._httpd
        thread = self._thread
        self._httpd = None
        self._thread = None
        if httpd:
            if thread is not threading.current_thread():
                httpd.shutdown()
            httpd.server_close()
        self.debugger.close()
        self.tasks.shutdown()
        if thread and thread is not threading.current_thread():
            thread.join(timeout=5)

    def status_snapshot(self) -> dict[str, Any]:
        return {
            "sessionId": self.session_id,
            "processId": os.getpid(),
            "endpoint": self.endpoint,
            "startedAtUnix": self._started_at,
            "roots": self.workspace.root_snapshot(),
            "runtime": self.runtime.health(),
            "toolchains": self.toolchains.snapshot(),
            "nativeDebugger": self.debugger.profile(),
            "processControl": self.processes.profile(),
            "processMemory": self.memory.profile(),
            "tasks": {
                "accepted": len(self.operations.list({"accepted"})),
                "running": len(self.operations.list({"running"})),
                "completed": len(self.operations.list({"succeeded", "failed", "partial", "indeterminate", "cancelled", "rejected"})),
            },
        }

    def dispatch(self, method: str, params: dict[str, Any]) -> dict[str, Any] | None:
        if method == "initialize":
            requested = params.get("protocolVersion")
            if not isinstance(requested, str) or not requested:
                raise ToolError(
                    "transport.invalid_arguments",
                    "initialize requires a non-empty protocolVersion",
                    stage="transport.initialize",
                )
            return {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {
                    "tools": {"listChanged": True},
                    "resources": {"subscribe": False, "listChanged": True},
                    "logging": {},
                },
                "serverInfo": {"name": "ue4ss-ai-mcp-host", "version": "1.0.0"},
                "instructions": "Use host status first. Discover and control games only through explicitly configured workspace roots; configure the embedded runtime endpoint separately.",
            }
        if method in {"notifications/initialized", "notifications/cancelled", "notifications/progress"}:
            return None
        if method == "ping":
            return {}
        if method == "tools/list":
            return {"tools": self._all_tools()}
        if method == "tools/call":
            name = self._required_string(params, "name")
            arguments = params.get("arguments", {})
            if not isinstance(arguments, dict):
                raise ToolError("transport.invalid_arguments", "Tool arguments must be an object", stage="transport.arguments")
            return self.call_tool(name, arguments)
        if method == "resources/list":
            resources = [
                {"uri": "ue4ss-host://status", "name": "Host status", "description": "Configured roots, runtime connection and toolchains", "mimeType": "application/json"},
                {"uri": "ue4ss-host://tasks", "name": "Host tasks", "description": "Current and completed host operations", "mimeType": "application/json"},
                {"uri": "ue4ss-host://events", "name": "Host events", "description": "Recent operation and validation events", "mimeType": "application/json"},
            ]
            try:
                resources.extend(self.runtime.resources())
            except ToolError:
                pass
            return {"resources": resources}
        if method == "resources/read":
            uri = self._required_string(params, "uri")
            if uri == "ue4ss-host://status":
                data = self.status_snapshot()
            elif uri == "ue4ss-host://tasks":
                data = self.operations.page(cursor=0, limit=100)
            elif uri == "ue4ss-host://events":
                data = self.events.read(0, 1000)
            else:
                return self.runtime.read_resource(uri)
            return {"contents": [{"uri": uri, "mimeType": "application/json", "text": json.dumps(data, ensure_ascii=False, separators=(",", ":"))}]}
        raise ToolError("transport.method_not_found", f"Unsupported MCP method '{method}'", stage="transport.dispatch", context={"method": method})

    def call_tool(self, name: str, arguments: dict[str, Any]) -> dict[str, Any]:
        try:
            if name not in self._host_tools:
                return self.runtime.call_tool(name, arguments)
            if name in RUNTIME_SCOPED_HOST_TOOLS:
                scope = arguments.get("scope", "host")
                if scope not in ("host", "runtime"):
                    raise ToolError(
                        "transport.invalid_arguments",
                        "Argument 'scope' must be host or runtime",
                        stage="transport.arguments",
                        context={"argument": "scope"},
                    )
                if name in ("ue4ss_task_get", "ue4ss_task_cancel"):
                    invalid_identifier = "operationId" if scope == "runtime" else "taskId"
                    if invalid_identifier in arguments:
                        raise ToolError(
                            "transport.invalid_arguments",
                            f"Argument '{invalid_identifier}' is not valid for {scope} scope",
                            stage="transport.arguments",
                            context={"argument": invalid_identifier, "scope": scope},
                        )
                if scope == "runtime":
                    return self.runtime.call_tool(name, {key: value for key, value in arguments.items() if key != "scope"})
            outcome = self._call_host_tool(name, arguments)
            if self._is_operation(outcome):
                operation = outcome
            else:
                operation = self._record_outcome(name, outcome)
            return content_result(operation)
        except ToolError as error:
            return content_result(self.operations.error_result(name, error), True)
        except Exception as error:
            tool_error = ToolError(
                "operation.internal_error",
                str(error),
                stage="tool.execute",
                context={"tool": name, "exceptionType": type(error).__name__},
            )
            return content_result(self.operations.error_result(name, tool_error), True)

    def _call_host_tool(self, name: str, arguments: dict[str, Any]) -> dict[str, Any]:
        if name == "ue4ss_host_status":
            return {"currentState": self.status_snapshot(), "evidence": [{"kind": "host_snapshot", "source": self.endpoint}]}
        if name == "ue4ss_process_profile":
            state = self.processes.profile()
            return {"currentState": state, "evidence": state["evidence"]}
        if name == "ue4ss_process_discover":
            state = self.processes.discover(
                root=self._optional_string(arguments, "root"),
                executable_name=self._optional_string(arguments, "executableName"),
                cursor=self._integer(arguments, "cursor", 0, 0, 2**31 - 1),
                limit=self._integer(arguments, "limit", 200, 1, 1000),
            )
            return {"currentState": state, "evidence": state["evidence"]}
        if name == "ue4ss_process_start":
            return self.processes.start(
                root=self._required_string(arguments, "root"),
                executable=self._required_string(arguments, "executable"),
                arguments=self._string_list(arguments, "arguments"),
                working_directory=self._optional_string(arguments, "workingDirectory"),
            )
        if name == "ue4ss_process_stop":
            return self.processes.stop(
                pid=self._integer(arguments, "processId", 0, 1, 2**31 - 1),
                force=self._boolean(arguments, "force", False),
                timeout_seconds=self._number(arguments, "timeoutSeconds", 5, 0.1, 60),
            )
        if name == "ue4ss_process_restart":
            return self.processes.restart(
                pid=self._integer(arguments, "processId", 0, 1, 2**31 - 1),
                force=self._boolean(arguments, "force", False),
                timeout_seconds=self._number(arguments, "timeoutSeconds", 5, 0.1, 60),
            )
        if name == "ue4ss_memory_profile":
            state = self.memory.profile()
            return {"currentState": state, "evidence": [{"kind": "memory_capability", "source": "host_platform", **state}]}
        if name == "ue4ss_memory_regions":
            state = self.memory.regions(
                self._integer(arguments, "processId", 0, 1, 0xFFFFFFFF),
                self._integer(arguments, "startAddress", 0, 0, 0xFFFFFFFFFFFFFFFF),
                self._integer(arguments, "size", 0, 1, 64 * 1024 * 1024),
                self._integer(arguments, "maxRegions", 1000, 1, 1000),
            )
            return {"currentState": state, "evidence": [{"kind": "memory_regions", "source": f"pid:{state['processId']}", "regionCount": state["regionCount"], "coveredBytes": state["coveredBytes"]}]}
        if name == "ue4ss_memory_read":
            state = self.memory.read(
                self._integer(arguments, "processId", 0, 1, 0xFFFFFFFF),
                self._integer(arguments, "address", 0, 0, 0xFFFFFFFFFFFFFFFF),
                self._integer(arguments, "size", 0, 1, 1024 * 1024),
            )
            return {"currentState": state, "evidence": [{"kind": "memory_read", "source": f"pid:{state['processId']}", "address": state["address"], "bytesRead": state["bytesRead"], "sha256": state["sha256"]}]}
        if name == "ue4ss_memory_write":
            process_id = self._integer(arguments, "processId", 0, 1, 0xFFFFFFFF)
            address = self._integer(arguments, "address", 0, 0, 0xFFFFFFFFFFFFFFFF)
            try:
                state = self.memory.write(process_id, address, self._required_string(arguments, "contentBase64"))
            except ToolError as error:
                if error.code not in {"memory.partial_write", "memory.write_verification_failed"}:
                    raise
                partial = error.code == "memory.partial_write"
                operation = self.operations.create(name, status=error.status, current_action=error.stage)
                return self.operations.update(
                    operation.operation_id,
                    progress=1.0,
                    current_state={"failureContext": error.context},
                    actual_changes=[{"kind": "process_memory", "action": "partially_written" if partial else "written_unverified", "processId": process_id, "address": address, "bytesWritten": error.context["bytesWritten"]}],
                    evidence=[{"kind": "memory_partial_write" if partial else "memory_write_verification_failed", "source": f"pid:{process_id}", **error.context}],
                    errors=[error.as_dict()],
                ).as_dict()
            return {
                "currentState": state,
                "actualChanges": [{"kind": "process_memory", "action": "written", "processId": state["processId"], "address": state["address"], "bytesWritten": state["bytesWritten"]}],
                "evidence": [{"kind": "memory_write_verification", "source": f"pid:{state['processId']}", "requestedSha256": state["requestedSha256"], "verifiedSha256": state["verifiedSha256"], "verifiedEqual": state["verifiedEqual"]}],
            }
        if name == "ue4ss_memory_pattern_scan":
            process_id = self._integer(arguments, "processId", 0, 1, 0xFFFFFFFF)
            start_address = self._integer(arguments, "startAddress", 0, 0, 0xFFFFFFFFFFFFFFFF)
            size = self._integer(arguments, "size", 0, 1, 64 * 1024 * 1024)
            pattern = self._required_string(arguments, "pattern")
            max_matches = self._integer(arguments, "maxMatches", 1000, 1, 1000)
            return self.tasks.submit(
                name,
                "scanning process memory",
                lambda context: self.memory.pattern_scan(context, process_id, start_address, size, pattern, max_matches),
            )
        if name == "ue4ss_tasks_list":
            statuses_value = arguments.get("statuses")
            statuses = set(statuses_value) if isinstance(statuses_value, list) else None
            return {
                "currentState": self.operations.page(
                    statuses=statuses,
                    cursor=self._integer(arguments, "cursor", 0, 0, 2**63 - 1),
                    limit=self._integer(arguments, "limit", 100, 1, 1000),
                )
            }
        if name == "ue4ss_task_get":
            operation = self.operations.get(self._required_string(arguments, "operationId"))
            return {"currentState": operation.as_dict()}
        if name == "ue4ss_task_cancel":
            return self.tasks.cancel(self._required_string(arguments, "operationId"))
        if name == "ue4ss_editor_profile":
            return {
                "currentState": self.toolchains.editor_profile(
                    root=self._optional_string(arguments, "root"),
                    project_path=self._optional_string(arguments, "projectPath"),
                    target_version=self._optional_string(arguments, "targetVersion"),
                )
            }
        if name == "ue4ss_events_read":
            return {
                "currentState": self.events.read(
                    self._integer(arguments, "cursor", 0, 0, 2**63 - 1),
                    self._integer(arguments, "limit", 100, 1, 1000),
                    self._integer(arguments, "waitMs", 0, 0, 30000) / 1000,
                )
            }
        if name == "ue4ss_settings_profile":
            return self.settings.profile(
                self._required_string(arguments, "root"),
                self._required_string(arguments, "path"),
                self._optional_string(arguments, "overridePath"),
            )
        if name == "ue4ss_settings_patch":
            return self.settings.patch(
                self._required_string(arguments, "root"),
                self._required_string(arguments, "path"),
                arguments.get("changes"),
                self._required_string(arguments, "expectedSha256"),
                self._optional_string(arguments, "overridePath"),
            )
        if name == "ue4ss_files_list":
            return {"currentState": self.workspace.list(self._required_string(arguments, "root"), self._string(arguments, "path", ""), self._integer(arguments, "cursor", 0, 0, 2**63 - 1), self._integer(arguments, "limit", 200, 1, 1000), self._boolean(arguments, "recursive", False))}
        if name == "ue4ss_files_read":
            return {"currentState": self.workspace.read(self._required_string(arguments, "root"), self._required_string(arguments, "path"), self._integer(arguments, "offset", 0, 0, 2**63 - 1), self._integer(arguments, "maxBytes", 65536, 1, 4194304), self._string(arguments, "encoding", "utf-8"))}
        if name == "ue4ss_files_search":
            return {"currentState": self.workspace.search(self._required_string(arguments, "root"), self._string(arguments, "path", ""), self._required_string(arguments, "pattern"), file_pattern=self._string(arguments, "filePattern", "*"), case_sensitive=self._boolean(arguments, "caseSensitive", True), limit=self._integer(arguments, "limit", 200, 1, 5000))}
        if name == "ue4ss_files_write":
            return self.workspace.write(self._required_string(arguments, "root"), self._required_string(arguments, "path"), self._required_string(arguments, "content", allow_empty=True), encoding=self._string(arguments, "encoding", "utf-8"), expected_sha256=self._optional_string(arguments, "expectedSha256"), create_parents=self._boolean(arguments, "createParents", True))
        if name == "ue4ss_files_move":
            return self.workspace.move(self._required_string(arguments, "root"), self._required_string(arguments, "source"), self._required_string(arguments, "destination"), overwrite=self._boolean(arguments, "overwrite", False))
        if name == "ue4ss_files_delete":
            return self.workspace.delete(self._required_string(arguments, "root"), self._required_string(arguments, "path"), recursive=self._boolean(arguments, "recursive", False), expected_sha256=self._optional_string(arguments, "expectedSha256"))
        if name == "ue4ss_files_compare":
            return {"currentState": self.workspace.compare(self._required_string(arguments, "leftRoot"), self._required_string(arguments, "leftPath"), self._required_string(arguments, "rightRoot"), self._required_string(arguments, "rightPath"), text=self._boolean(arguments, "text", False))}
        if name == "ue4ss_files_backup":
            items = arguments.get("items")
            if not isinstance(items, list) or not all(isinstance(item, dict) for item in items):
                raise ToolError("transport.invalid_arguments", "items must be an array of path objects", stage="transport.arguments")
            return self.workspace.backup(self._required_string(arguments, "name"), items)
        if name == "ue4ss_files_restore":
            return self.workspace.restore(self._required_string(arguments, "name"), overwrite=self._boolean(arguments, "overwrite", False))
        if name == "ue4ss_artifacts_manifest":
            return {"currentState": self.workspace.manifest(self._required_string(arguments, "root"), self._string(arguments, "path", ""), recursive=self._boolean(arguments, "recursive", True), limit=self._integer(arguments, "limit", 5000, 1, 10000))}
        if name == "ue4ss_mod_create":
            mod_type = self._required_string(arguments, "type")
            common = {"root": self._required_string(arguments, "root"), "directory": self._string(arguments, "directory", ""), "name": self._required_string(arguments, "name"), "overwrite": self._boolean(arguments, "overwrite", False)}
            if mod_type == "lua":
                metadata = arguments.get("metadata")
                if metadata is not None and not isinstance(metadata, dict):
                    raise ToolError("transport.invalid_arguments", "metadata must be an object", stage="transport.arguments")
                return self.mods.create_lua(**common, entry_script=self._optional_string(arguments, "entryScript"), metadata=metadata)
            if mod_type == "cpp":
                return self.mods.create_cpp(**common, ue4ss_source=self._required_string(arguments, "ue4ssSource"), description=self._string(arguments, "description", "UE4SS C++ mod"), author=self._string(arguments, "author", "UE4SS Mod Developer"))
            raise ToolError("transport.invalid_arguments", "type must be lua or cpp", stage="transport.arguments")
        if name == "ue4ss_mod_install":
            return self.mods.install(self._required_string(arguments, "sourceRoot"), self._required_string(arguments, "sourcePath"), self._required_string(arguments, "destinationRoot"), self._required_string(arguments, "modsPath"), self._required_string(arguments, "name"), overwrite=self._boolean(arguments, "overwrite", False))
        if name == "ue4ss_mod_set_enabled":
            return self.mods.set_enabled(self._required_string(arguments, "root"), self._required_string(arguments, "modsPath"), self._required_string(arguments, "name"), self._required_boolean(arguments, "enabled"))
        if name == "ue4ss_mod_remove":
            expected = arguments.get("expectedManifest")
            if expected is not None and not isinstance(expected, list):
                raise ToolError("transport.invalid_arguments", "expectedManifest must be an array", stage="transport.arguments")
            return self.mods.remove(self._required_string(arguments, "root"), self._required_string(arguments, "modsPath"), self._required_string(arguments, "name"), expected_manifest=expected)
        if name == "ue4ss_cpp_build":
            return self.tasks.submit(
                name,
                "configuring C++ build",
                lambda context: self.toolchains.cmake_build_task(
                    context,
                    root=self._required_string(arguments, "root"),
                    source_path=self._string(arguments, "sourcePath", ""),
                    build_path=self._required_string(arguments, "buildPath"),
                    target=self._optional_string(arguments, "target"),
                    configure_args=self._string_list(arguments, "configureArgs"),
                    build_args=self._string_list(arguments, "buildArgs"),
                    install_path=self._optional_string(arguments, "installPath"),
                    timeout_seconds=self._number(arguments, "timeoutSeconds", 1800, 1, 7200),
                ),
            )
        if name == "ue4ss_editor_run_script":
            return self.tasks.submit(
                name,
                "running Unreal Editor",
                lambda context: self.toolchains.run_editor_script_task(
                    context,
                    root=self._required_string(arguments, "root"),
                    project_path=self._required_string(arguments, "projectPath"),
                    script_path=self._required_string(arguments, "scriptPath"),
                    arguments=self._string_list(arguments, "arguments"),
                    timeout_seconds=self._number(arguments, "timeoutSeconds", 3600, 1, 14400),
                ),
            )
        if name == "ue4ss_editor_cook":
            return self.tasks.submit(
                name,
                "cooking Unreal content",
                lambda context: self.toolchains.editor_cook_task(
                    context,
                    root=self._required_string(arguments, "root"),
                    project_path=self._required_string(arguments, "projectPath"),
                    target_platform=self._required_string(arguments, "targetPlatform"),
                    maps=self._string_list(arguments, "maps"),
                    cook_directories=self._string_list(arguments, "cookDirectories"),
                    output_path=self._optional_string(arguments, "outputPath"),
                    arguments=self._string_list(arguments, "arguments"),
                    timeout_seconds=self._number(arguments, "timeoutSeconds", 3600, 1, 14400),
                ),
            )
        if name == "ue4ss_editor_package":
            command_name = self._required_string(arguments, "command")
            if command_name not in {"BuildCookRun", "BuildPlugin"}:
                raise ToolError("transport.invalid_arguments", "command must be BuildCookRun or BuildPlugin", stage="transport.arguments")
            return self.tasks.submit(
                name,
                "packaging Unreal project",
                lambda context: self.toolchains.run_uat_task(
                    context,
                    root=self._required_string(arguments, "root"),
                    project_path=self._required_string(arguments, "projectPath"),
                    command_name=command_name,
                    arguments=self._string_list(arguments, "arguments"),
                    artifact_path=self._optional_string(arguments, "artifactPath"),
                    timeout_seconds=self._number(arguments, "timeoutSeconds", 3600, 1, 14400),
                ),
            )
        if name == "ue4ss_unrealpak_pack":
            entries = arguments.get("entries")
            if not isinstance(entries, list) or not all(
                isinstance(item, dict) and isinstance(item.get("sourcePath"), str) and isinstance(item.get("mountPath"), str)
                for item in entries
            ):
                raise ToolError("transport.invalid_arguments", "entries must contain sourcePath and mountPath strings", stage="transport.arguments")
            return self.tasks.submit(
                name,
                "building Unreal package",
                lambda context: self.toolchains.unrealpak_task(
                    context,
                    root=self._required_string(arguments, "root"),
                    output_path=self._required_string(arguments, "outputPath"),
                    entries=entries,
                    arguments=self._string_list(arguments, "arguments"),
                    timeout_seconds=self._number(arguments, "timeoutSeconds", 1800, 1, 7200),
                ),
            )
        if name == "ue4ss_crash_artifacts":
            locations = arguments.get("locations")
            if not isinstance(locations, list) or not all(isinstance(item, dict) and isinstance(item.get("root"), str) and isinstance(item.get("path"), str) for item in locations):
                raise ToolError("transport.invalid_arguments", "locations must contain root/path objects", stage="transport.arguments")
            return {"currentState": self.toolchains.crash_artifacts([(item["root"], item["path"]) for item in locations])}
        if name == "ue4ss_native_debug_profile":
            return {"currentState": self.debugger.profile()}
        if name == "ue4ss_native_debug_sessions":
            return {"currentState": {"sessions": self.debugger.list()}}
        if name == "ue4ss_native_debug_attach":
            process_id = self._integer(arguments, "processId", 0, 1, 2**31 - 1)
            attach_arguments = arguments.get("attachArguments", {})
            if not isinstance(attach_arguments, dict):
                raise ToolError("transport.invalid_arguments", "attachArguments must be an object", stage="transport.arguments")
            state = self.debugger.attach(
                process_id,
                self._string_list(arguments, "adapterArguments"),
                attach_arguments,
                self._number(arguments, "timeoutSeconds", 20, 1, 120),
                self._number(arguments, "leaseSeconds", 60, 1, 3600),
            )
            return {
                "currentState": state,
                "actualChanges": [{"kind": "debug_session", "action": "attached", "sessionId": state["sessionId"], "processId": process_id}],
                "evidence": [{"kind": "dap_session", "source": state["adapter"], "session": state}],
            }
        if name == "ue4ss_native_debug_breakpoints_set":
            session = self.debugger.get(self._required_string(arguments, "sessionId"))
            source = self.workspace.resolve(self._required_string(arguments, "root"), self._required_string(arguments, "path"), must_exist=True)
            if not source.path.is_file():
                raise ToolError("artifact.not_file", "Breakpoint source path is not a file", stage="debug.breakpoints")
            breakpoints = arguments.get("breakpoints")
            if not isinstance(breakpoints, list) or not all(isinstance(item, dict) for item in breakpoints):
                raise ToolError("transport.invalid_arguments", "breakpoints must be an array of objects", stage="transport.arguments")
            response = session.request(
                "setBreakpoints",
                {
                    "source": {"name": source.path.name, "path": str(source.path)},
                    "breakpoints": breakpoints,
                    "sourceModified": self._boolean(arguments, "sourceModified", False),
                },
            )
            evidence = [{"kind": "dap_response", "source": "setBreakpoints", "response": response}]
            configuration_state: dict[str, Any] | None = None
            if self._boolean(arguments, "configurationDone", True):
                configuration_response = session.configuration_done()
                configuration_state = configuration_response.get("body", {})
                evidence.append({"kind": "dap_response", "source": "configurationDone", "response": configuration_response})
            state = {
                "sessionId": session.session_id,
                "source": source.relative,
                **response.get("body", {}),
                "configurationDone": configuration_state,
            }
            return {
                "currentState": state,
                "actualChanges": [{"kind": "debug_breakpoints", "action": "replaced", "source": source.relative, "count": len(breakpoints)}],
                "evidence": evidence,
            }
        if name == "ue4ss_native_debug_stack":
            session = self.debugger.get(self._required_string(arguments, "sessionId"))
            threads_response = session.request("threads")
            state: dict[str, Any] = {"sessionId": session.session_id, "threads": threads_response.get("body", {}).get("threads", [])}
            thread_id = self._optional_integer(arguments, "threadId", 1, 2**31 - 1)
            evidence = [{"kind": "dap_response", "source": "threads", "response": threads_response}]
            if thread_id is not None:
                stack_response = session.request(
                    "stackTrace",
                    {
                        "threadId": thread_id,
                        "startFrame": self._integer(arguments, "startFrame", 0, 0, 2**31 - 1),
                        "levels": self._integer(arguments, "levels", 100, 1, 1000),
                        "format": {"parameters": True, "parameterTypes": True, "parameterNames": True, "line": True, "module": True},
                    },
                )
                state["stackTrace"] = stack_response.get("body", {})
                evidence.append({"kind": "dap_response", "source": "stackTrace", "response": stack_response})
            return {"currentState": state, "evidence": evidence}
        if name == "ue4ss_native_debug_variables":
            session = self.debugger.get(self._required_string(arguments, "sessionId"))
            frame_id = self._optional_integer(arguments, "frameId", 0, 2**31 - 1)
            variables_reference = self._optional_integer(arguments, "variablesReference", 0, 2**31 - 1)
            if (frame_id is None) == (variables_reference is None):
                raise ToolError("transport.invalid_arguments", "Supply exactly one of frameId or variablesReference", stage="transport.arguments")
            if frame_id is not None:
                response = session.request("scopes", {"frameId": frame_id})
                state = {"sessionId": session.session_id, "frameId": frame_id, **response.get("body", {})}
                source_name = "scopes"
            else:
                start = self._integer(arguments, "start", 0, 0, 2**31 - 1)
                count = self._integer(arguments, "count", 100, 1, 1000)
                response = session.request(
                    "variables",
                    {
                        "variablesReference": variables_reference,
                        "start": start,
                        "count": count,
                    },
                )
                body = response.get("body", {})
                variables = body.get("variables") if isinstance(body, dict) else None
                if not isinstance(variables, list):
                    raise ToolError(
                        "debug.adapter_protocol",
                        "DAP variables response must contain a variables array",
                        stage="debug.variables",
                        context={"sessionId": session.session_id, "response": response},
                    )
                adapter_returned_count = len(variables)
                truncated = adapter_returned_count > count
                bounded_body = {**body, "variables": variables[:count]}
                bounded_response = {**response, "body": bounded_body}
                returned = len(bounded_body["variables"])
                state = {
                    "sessionId": session.session_id,
                    "variablesReference": variables_reference,
                    **bounded_body,
                    "start": start,
                    "requestedCount": count,
                    "returned": returned,
                    "returnedCount": returned,
                    "adapterReturnedCount": adapter_returned_count,
                    "truncated": truncated,
                    "adapterIgnoredPaging": truncated,
                    "nextStart": start + returned if returned == count else None,
                }
                total = body.get("totalVariables")
                if isinstance(total, int) and total >= 0:
                    state["total"] = total
                response = bounded_response
                source_name = "variables"
            return {"currentState": state, "evidence": [{"kind": "dap_response", "source": source_name, "response": response}]}
        if name == "ue4ss_native_debug_evaluate":
            session = self.debugger.get(self._required_string(arguments, "sessionId"))
            request_arguments: dict[str, Any] = {
                "expression": self._required_string(arguments, "expression"),
                "context": self._string(arguments, "context", "repl"),
            }
            frame_id = self._optional_integer(arguments, "frameId", 0, 2**31 - 1)
            if frame_id is not None:
                request_arguments["frameId"] = frame_id
            response = session.request("evaluate", request_arguments)
            return {
                "currentState": {"sessionId": session.session_id, **response.get("body", {})},
                "evidence": [{"kind": "dap_response", "source": "evaluate", "response": response}],
            }
        if name == "ue4ss_native_debug_control":
            session = self.debugger.get(self._required_string(arguments, "sessionId"))
            command = self._required_string(arguments, "command")
            request_arguments: dict[str, Any] = {}
            if command == "configurationDone":
                response = session.configuration_done()
                changed = bool(response.get("body", {}).get("sent"))
            else:
                thread_id = self._optional_integer(arguments, "threadId", 1, 2**31 - 1)
                if thread_id is None:
                    raise ToolError("transport.invalid_arguments", "threadId is required for pause, continue and step commands", stage="transport.arguments")
                request_arguments = {"threadId": thread_id, "singleThread": self._boolean(arguments, "singleThread", False)}
                response = session.request(command, request_arguments)
                changed = True
            return {
                "currentState": {"sessionId": session.session_id, "command": command, **response.get("body", {})},
                "actualChanges": [{"kind": "debug_execution", "action": command, **request_arguments}] if changed else [],
                "evidence": [{"kind": "dap_response", "source": command, "response": response}],
            }
        if name == "ue4ss_native_debug_detach":
            session_id = self._required_string(arguments, "sessionId")
            terminate = self._boolean(arguments, "terminateDebuggee", False)
            state = self.debugger.detach(session_id, terminate_debuggee=terminate)
            return {
                "currentState": state,
                "actualChanges": [{"kind": "debug_session", "action": "terminated_target" if terminate else "detached", "sessionId": session_id}],
            }
        if name == "ue4ss_scenario_run":
            scenario = arguments.get("scenario")
            if not isinstance(scenario, dict):
                raise ToolError("transport.invalid_arguments", "scenario must be an object", stage="transport.arguments")
            return self.tasks.submit(name, "validating scenario", lambda context: self.scenarios.run(context, scenario))
        raise ToolError("transport.tool_not_found", f"Unknown host tool '{name}'", stage="tool.dispatch")

    def _all_tools(self) -> list[dict[str, Any]]:
        runtime_tools: list[dict[str, Any]] = []
        try:
            runtime_tools = self.runtime.tools()
            self._runtime_tool_cache = {tool["name"]: tool for tool in runtime_tools if isinstance(tool, dict) and isinstance(tool.get("name"), str)}
        except ToolError:
            runtime_tools = list(self._runtime_tool_cache.values())
        merged = dict(self._runtime_tool_cache)
        merged.update(self._host_tools)
        return [merged[name] for name in sorted(merged)]

    def _record_outcome(self, target: str, outcome: dict[str, Any]) -> dict[str, Any]:
        status = outcome.get("status", "succeeded")
        operation = self.operations.create(target, status=status, current_action="completed")
        if status == "succeeded":
            return self.operations.succeed(
                operation.operation_id,
                state=outcome.get("currentState"),
                changes=outcome.get("actualChanges"),
                evidence=outcome.get("evidence"),
                restart_required=bool(outcome.get("restartRequired")),
                reload_required=bool(outcome.get("reloadRequired")),
            ).as_dict()
        return self.operations.update(
            operation.operation_id,
            status=status,
            progress=1.0,
            current_state=outcome.get("currentState", {}),
            actual_changes=outcome.get("actualChanges", []),
            evidence=outcome.get("evidence", []),
            errors=outcome.get("errors", []),
            suggested_actions=outcome.get("suggestedActions", []),
            restart_required=bool(outcome.get("restartRequired")),
            reload_required=bool(outcome.get("reloadRequired")),
        ).as_dict()

    @staticmethod
    def _is_operation(value: dict[str, Any]) -> bool:
        return isinstance(value.get("operationId"), str) and isinstance(value.get("status"), str)

    def _make_handler(self) -> type[BaseHTTPRequestHandler]:
        owner = self

        class Handler(BaseHTTPRequestHandler):
            server_version = "UE4SSMcpHost/1.0"
            protocol_version = "HTTP/1.1"

            def do_GET(self) -> None:
                parsed = urlsplit(self.path)
                if parsed.path == "/health":
                    self._json(HTTPStatus.OK, {"status": "ok", "protocolVersion": PROTOCOL_VERSION, "sessionId": owner.session_id})
                    return
                if parsed.path != "/mcp":
                    self._json(HTTPStatus.NOT_FOUND, {"error": "not_found"})
                    return
                if not self._origin_allowed():
                    self._json(HTTPStatus.FORBIDDEN, {"error": "origin_not_allowed"})
                    return
                cursor_value = parse_qs(parsed.query).get("cursor", ["0"])[0]
                try:
                    cursor = max(int(cursor_value), 0)
                except ValueError:
                    self._json(HTTPStatus.BAD_REQUEST, {"error": "invalid_cursor"})
                    return
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Connection", "keep-alive")
                self.end_headers()
                try:
                    while owner._httpd is not None:
                        page = owner.events.read(cursor, 100, 15)
                        if not page["events"]:
                            self.wfile.write(b": keepalive\n\n")
                            self.wfile.flush()
                            continue
                        for event in page["events"]:
                            cursor = event["sequence"]
                            notification = {
                                "jsonrpc": "2.0",
                                "method": "notifications/message",
                                "params": {"level": "info", "logger": "ue4ss-ai-mcp-host", "data": event},
                            }
                            payload = json.dumps(notification, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
                            self.wfile.write(f"id: {cursor}\nevent: message\ndata: ".encode("utf-8") + payload + b"\n\n")
                        self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError, OSError):
                    return

            def do_POST(self) -> None:
                if urlsplit(self.path).path != "/mcp":
                    self._json(HTTPStatus.NOT_FOUND, {"error": "not_found"})
                    return
                if not self._origin_allowed():
                    self._json(HTTPStatus.FORBIDDEN, {"error": "origin_not_allowed"})
                    return
                protocol = self.headers.get("MCP-Protocol-Version")
                try:
                    length = int(self.headers.get("Content-Length", "0"))
                except ValueError:
                    self._rpc_error(HTTPStatus.BAD_REQUEST, None, -32600, "Invalid Content-Length", {"code": "transport.invalid_request"})
                    return
                if length <= 0 or length > MAX_REQUEST_BYTES:
                    self._rpc_error(HTTPStatus.BAD_REQUEST, None, -32600, "Invalid request size", {"code": "transport.invalid_request"})
                    return
                try:
                    request = json.loads(self.rfile.read(length))
                except json.JSONDecodeError:
                    self._rpc_error(HTTPStatus.BAD_REQUEST, None, -32700, "Parse error", {"code": "transport.invalid_json"})
                    return
                if not isinstance(request, dict) or request.get("jsonrpc") != "2.0" or not isinstance(request.get("method"), str):
                    self._rpc_error(HTTPStatus.BAD_REQUEST, request.get("id") if isinstance(request, dict) else None, -32600, "Invalid Request", {"code": "transport.invalid_request"})
                    return
                if request["method"] != "initialize" and protocol != PROTOCOL_VERSION:
                    self._rpc_error(HTTPStatus.BAD_REQUEST, request.get("id"), -32600, "Protocol version mismatch", {"code": "transport.protocol_mismatch"})
                    return
                request_id = request.get("id")
                params = request.get("params", {})
                if not isinstance(params, dict):
                    self._rpc_error(HTTPStatus.BAD_REQUEST, request_id, -32602, "Invalid params", {"code": "transport.invalid_arguments"})
                    return
                try:
                    result = owner.dispatch(request["method"], params)
                    if request_id is None:
                        self.send_response(HTTPStatus.ACCEPTED)
                        self.send_header("Content-Length", "0")
                        self.end_headers()
                    else:
                        self._json(HTTPStatus.OK, {"jsonrpc": "2.0", "id": request_id, "result": result or {}})
                except ToolError as error:
                    code = -32601 if error.code == "transport.method_not_found" else -32602 if error.code == "transport.invalid_arguments" else -32000
                    self._rpc_error(HTTPStatus.OK, request_id, code, str(error), error.as_dict())
                except Exception as error:
                    self._rpc_error(HTTPStatus.INTERNAL_SERVER_ERROR, request_id, -32603, "Internal error", {"code": "operation.internal_error", "message": str(error)})

            def do_PUT(self) -> None:
                self._method_not_allowed()

            def do_DELETE(self) -> None:
                self._method_not_allowed()

            def log_message(self, format_: str, *args: Any) -> None:
                return

            def _method_not_allowed(self) -> None:
                self.send_response(HTTPStatus.METHOD_NOT_ALLOWED)
                self.send_header("Allow", "GET, POST")
                self.send_header("Content-Length", "0")
                self.end_headers()

            def _origin_allowed(self) -> bool:
                origin = self.headers.get("Origin")
                if not origin:
                    return True
                try:
                    host = urlsplit(origin).hostname
                except ValueError:
                    return False
                return host in {"127.0.0.1", "localhost", "::1"}

            def _rpc_error(self, status: HTTPStatus, request_id: Any, code: int, message: str, data: dict[str, Any]) -> None:
                self._json(status, {"jsonrpc": "2.0", "id": request_id, "error": {"code": code, "message": message, "data": data}})

            def _json(self, status: HTTPStatus, value: dict[str, Any]) -> None:
                body = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        return Handler

    @staticmethod
    def _required_string(arguments: dict[str, Any], name: str, *, allow_empty: bool = False) -> str:
        value = arguments.get(name)
        if not isinstance(value, str) or (not allow_empty and not value):
            raise ToolError("transport.invalid_arguments", f"Argument '{name}' must be a string", stage="transport.arguments", context={"argument": name})
        return value

    @staticmethod
    def _string(arguments: dict[str, Any], name: str, fallback: str) -> str:
        value = arguments.get(name, fallback)
        if not isinstance(value, str):
            raise ToolError("transport.invalid_arguments", f"Argument '{name}' must be a string", stage="transport.arguments", context={"argument": name})
        return value

    @staticmethod
    def _optional_string(arguments: dict[str, Any], name: str) -> str | None:
        value = arguments.get(name)
        if value is not None and not isinstance(value, str):
            raise ToolError("transport.invalid_arguments", f"Argument '{name}' must be a string or null", stage="transport.arguments", context={"argument": name})
        return value

    @staticmethod
    def _boolean(arguments: dict[str, Any], name: str, fallback: bool) -> bool:
        value = arguments.get(name, fallback)
        if not isinstance(value, bool):
            raise ToolError("transport.invalid_arguments", f"Argument '{name}' must be a boolean", stage="transport.arguments", context={"argument": name})
        return value

    @staticmethod
    def _required_boolean(arguments: dict[str, Any], name: str) -> bool:
        if name not in arguments:
            raise ToolError("transport.invalid_arguments", f"Missing boolean argument '{name}'", stage="transport.arguments", context={"argument": name})
        return McpHostServer._boolean(arguments, name, False)

    @staticmethod
    def _integer(arguments: dict[str, Any], name: str, fallback: int, minimum: int, maximum: int) -> int:
        value = arguments.get(name, fallback)
        if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
            raise ToolError("transport.invalid_arguments", f"Argument '{name}' must be an integer in [{minimum}, {maximum}]", stage="transport.arguments", context={"argument": name})
        return value

    @staticmethod
    def _optional_integer(arguments: dict[str, Any], name: str, minimum: int, maximum: int) -> int | None:
        value = arguments.get(name)
        if value is None:
            return None
        if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
            raise ToolError("transport.invalid_arguments", f"Argument '{name}' must be an integer or null in [{minimum}, {maximum}]", stage="transport.arguments", context={"argument": name})
        return value

    @staticmethod
    def _number(arguments: dict[str, Any], name: str, fallback: float, minimum: float, maximum: float) -> float:
        value = arguments.get(name, fallback)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not minimum <= value <= maximum:
            raise ToolError("transport.invalid_arguments", f"Argument '{name}' must be a number in [{minimum}, {maximum}]", stage="transport.arguments", context={"argument": name})
        return float(value)

    @staticmethod
    def _string_list(arguments: dict[str, Any], name: str) -> list[str]:
        value = arguments.get(name, [])
        if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
            raise ToolError("transport.invalid_arguments", f"Argument '{name}' must be an array of strings", stage="transport.arguments", context={"argument": name})
        return list(value)
