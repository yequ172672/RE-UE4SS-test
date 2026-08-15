from __future__ import annotations

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import base64
import ctypes
import json
import os
from pathlib import Path
import tempfile
import sys
import threading
import time
import unittest
from unittest import mock
import urllib.request

from tools.mcp_host.model import OperationStore, ToolError
from tools.mcp_host.server import McpHostServer
from tools.mcp_host.settings import SettingsManager
from tools.mcp_host.tasks import TaskRegistry

from tools.mcp_host.workspace import Workspace

class FakeRuntime:
    def __init__(self) -> None:
        self.calls: list[tuple[str, dict[str, object]]] = []
        runtime_task = {
            "taskId": "runtime-test-task-1",
            "taskType": "dumper",
            "kind": "objects",
            "status": "running",
            "progress": 0.5,
            "currentAction": "running objects dumper",
            "actualChanges": [{"kind": "file", "path": "ObjectDump.txt"}],
            "result": {"artifactCount": 1},
        }
        self.tool_results: dict[str, dict[str, object]] = {
            "ue4ss_tasks_list": {"tasks": [runtime_task], "count": 1},
            "ue4ss_task_get": runtime_task,
            "ue4ss_task_cancel": {
                **runtime_task,
                "cancelRequested": True,
                "cancellationAccepted": False,
                "cancellationMessage": "The dumper entrance has begun and is non-cooperative; execution will continue",
            },
            "ue4ss_events_read": {
                "events": [{"sequence": 7, "kind": "task.completed", "data": {"taskId": runtime_task["taskId"]}}],
                "oldestCursor": 0,
                "nextCursor": 7,
                "newestSequence": 7,
                "cursorExpired": False,
                "truncated": False,
            },
        }
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self) -> None:
                if self.path == "/health":
                    self._send({"status": "ok", "sessionId": "runtime-test"})
                else:
                    self.send_error(404)

            def do_POST(self) -> None:
                request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                method = request["method"]
                params = request.get("params", {})
                owner.calls.append((method, params))
                if method == "initialize":
                    result = {"protocolVersion": "2025-06-18", "serverInfo": {"name": "fake-runtime", "version": "1"}, "capabilities": {"tools": {}, "resources": {}}}
                elif method == "tools/list":
                    result = {"tools": [{"name": "ue4ss_fake_runtime", "description": "Fake runtime tool", "inputSchema": {"type": "object"}}]}
                elif method == "tools/call":
                    tool_name = params["name"]
                    structured = owner.tool_results.get(tool_name, {"runtime": True, "arguments": params["arguments"]})
                    result = {"content": [{"type": "text", "text": json.dumps(structured)}], "structuredContent": structured, "isError": False}
                elif method == "resources/list":
                    result = {"resources": [{"uri": "ue4ss://fake", "name": "Fake", "mimeType": "application/json"}]}
                elif method == "resources/read":
                    result = {"contents": [{"uri": params["uri"], "mimeType": "application/json", "text": "{}"}]}
                else:
                    self._send({"jsonrpc": "2.0", "id": request.get("id"), "error": {"code": -32601, "message": "missing"}})
                    return
                self._send({"jsonrpc": "2.0", "id": request.get("id"), "result": result})

            def log_message(self, format_: str, *args: object) -> None:
                return

            def _send(self, value: dict[str, object]) -> None:
                body = json.dumps(value).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def endpoint(self) -> str:
        return f"http://127.0.0.1:{self.server.server_address[1]}/mcp"

    def close(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)


class McpHostTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        base = Path(self.temporary.name)
        self.source = base / "source"
        self.game = base / "game"
        self.source.mkdir()
        (self.game / "Mods").mkdir(parents=True)
        self.adapter = base / "fake_dap.py"
        self.adapter.write_text(
            '''import json, sys
sequence = 1
no_configuration_done = "--no-config" in sys.argv[1:]
log_path = next((argument for argument in sys.argv[1:] if argument != "--no-config"), None)
def send(message):
    global sequence
    message["seq"] = sequence
    sequence += 1
    payload = json.dumps(message, separators=(",", ":")).encode()
    sys.stdout.buffer.write(f"Content-Length: {len(payload)}\\r\\n\\r\\n".encode() + payload)
    sys.stdout.buffer.flush()
while True:
    headers = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            raise SystemExit
        if line in (b"\\r\\n", b"\\n"):
            break
        key, value = line.decode().split(":", 1)
        headers[key.lower()] = value.strip()
    request = json.loads(sys.stdin.buffer.read(int(headers["content-length"])))
    command = request["command"]
    arguments = request.get("arguments", {})
    if log_path:
        with open(log_path, "a", encoding="utf-8") as log:
            log.write(command + "\\n")
    body = {}
    if command == "initialize":
        body = {"supportsConfigurationDoneRequest": not no_configuration_done, "supportsEvaluateForHovers": True}
    elif command == "setBreakpoints":
        body = {"breakpoints": [{"id": index + 1, "verified": True, "line": item["line"]} for index, item in enumerate(arguments["breakpoints"])]}
    elif command == "threads":
        body = {"threads": [{"id": 1, "name": "GameThread"}]}
    elif command == "stackTrace":
        body = {"stackFrames": [{"id": 10, "name": "Tick", "line": 7, "column": 1, "source": {"path": "debug.cpp"}}], "totalFrames": 1}
    elif command == "scopes":
        body = {"scopes": [{"name": "Locals", "variablesReference": 20, "expensive": False}]}
    elif command == "variables":
        if arguments["variablesReference"] == 99:
            body = {"variables": [{"name": f"Value{index}", "value": str(index), "type": "int", "variablesReference": 0} for index in range(150)]}
        else:
            body = {"variables": [{"name": "Counter", "value": "1", "type": "int", "variablesReference": 0}]}
    elif command == "evaluate":
        body = {"result": "1", "type": "int", "variablesReference": 0}
    send({"type": "response", "request_seq": request["seq"], "command": command, "success": True, "body": body})
    if command == "attach":
        send({"type": "event", "event": "output", "body": {"category": "console", "output": "adapter attached\\n"}})
        send({"type": "event", "event": "initialized", "body": {}})
        send({"type": "event", "event": "stopped", "body": {"reason": "breakpoint", "threadId": 1, "allThreadsStopped": True}})
    if command == "continue":
        send({"type": "event", "event": "continued", "body": {"threadId": arguments["threadId"], "allThreadsContinued": True}})
    if command == "disconnect":
        break
''',
            encoding="utf-8",
        )
        (self.source / "debug.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
        self.started_process_ids: list[int] = []
        self.runtime = FakeRuntime()
        self.host = McpHostServer(
            roots={"source": self.source, "game": self.game, "python": Path(sys.executable).parent},
            state_directory=base / "state",
            runtime_endpoint=self.runtime.endpoint,
            port=0,
            tools={"debugger": sys.executable},
        )
        self.host.start()
        self.request_id = 1


    def tearDown(self) -> None:
        for pid in self.started_process_ids:
            self.call("ue4ss_process_stop", {"processId": pid, "force": True, "timeoutSeconds": 0.2})
        self.host.close()
        self.runtime.close()
        self.temporary.cleanup()

    def rpc(self, method: str, params: dict[str, object] | None = None) -> dict[str, object]:
        request_id = self.request_id
        self.request_id += 1
        data = json.dumps({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params or {}}).encode()
        request = urllib.request.Request(
            self.host.endpoint,
            data=data,
            method="POST",
            headers={"Content-Type": "application/json", "MCP-Protocol-Version": "2025-06-18"},
        )
        with urllib.request.urlopen(request, timeout=10) as response:
            self.assertEqual(response.status, 200)
            return json.loads(response.read())

    def call(self, name: str, arguments: dict[str, object]) -> dict[str, object]:
        envelope = self.rpc("tools/call", {"name": name, "arguments": arguments})
        return envelope["result"]["structuredContent"]

    def wait_operation(self, operation_id: str, timeout: float = 10) -> dict[str, object]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            operation = self.call("ue4ss_task_get", {"operationId": operation_id})["currentState"]
            if operation["status"] not in {"accepted", "running"}:
                return operation
            time.sleep(0.02)
        self.fail(f"operation {operation_id} did not complete")

    def test_transport_discovery_and_runtime_proxy(self) -> None:
        initialized = self.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}})
        self.assertEqual(initialized["result"]["serverInfo"]["name"], "ue4ss-ai-mcp-host")
        negotiation_request = urllib.request.Request(
            self.host.endpoint,
            data=json.dumps(
                {
                    "jsonrpc": "2.0",
                    "id": "negotiate",
                    "method": "initialize",
                    "params": {"protocolVersion": "2099-01-01", "capabilities": {}, "clientInfo": {"name": "newer", "version": "1"}},
                }
            ).encode(),
            method="POST",
            headers={"Content-Type": "application/json", "MCP-Protocol-Version": "2099-01-01"},
        )
        with urllib.request.urlopen(negotiation_request, timeout=10) as response:
            negotiated = json.loads(response.read())
        self.assertEqual(negotiated["result"]["protocolVersion"], "2025-06-18")
        tools = self.rpc("tools/list")["result"]["tools"]
        names = {tool["name"] for tool in tools}
        self.assertIn("ue4ss_files_write", names)
        self.assertIn("ue4ss_process_discover", names)
        self.assertIn("ue4ss_process_restart", names)
        self.assertIn("ue4ss_memory_profile", names)
        self.assertIn("ue4ss_memory_regions", names)
        self.assertIn("ue4ss_memory_read", names)
        self.assertIn("ue4ss_memory_write", names)
        self.assertIn("ue4ss_memory_pattern_scan", names)
        self.assertIn("ue4ss_fake_runtime", names)
        proxied = self.rpc("tools/call", {"name": "ue4ss_fake_runtime", "arguments": {"value": 42}})["result"]
        self.assertEqual(proxied["structuredContent"], {"runtime": True, "arguments": {"value": 42}})
        resources = self.rpc("resources/list")["result"]["resources"]
        self.assertIn("ue4ss://fake", {resource["uri"] for resource in resources})
        editor = self.call("ue4ss_editor_profile", {})
        self.assertFalse(editor["currentState"]["available"])
        self.assertTrue(editor["currentState"]["missingConditions"])

    def test_scoped_task_and_event_schemas_distinguish_host_and_runtime(self) -> None:
        tools = {tool["name"]: tool for tool in self.rpc("tools/list")["result"]["tools"]}
        for name in ("ue4ss_tasks_list", "ue4ss_task_get", "ue4ss_task_cancel", "ue4ss_events_read"):
            scope = tools[name]["inputSchema"]["properties"]["scope"]
            self.assertEqual(scope, {"type": "string", "enum": ["host", "runtime"], "default": "host"})
        for name in ("ue4ss_task_get", "ue4ss_task_cancel"):
            schema = tools[name]["inputSchema"]
            self.assertIn("operationId", schema["properties"])
            self.assertIn("taskId", schema["properties"])
            self.assertEqual(schema["then"]["required"], ["taskId"])
            self.assertEqual(schema["then"]["not"]["required"], ["operationId"])
            self.assertEqual(schema["else"]["required"], ["operationId"])
            self.assertEqual(schema["else"]["not"]["required"], ["taskId"])

    def test_runtime_scope_routes_all_colliding_tools_and_preserves_payloads(self) -> None:
        calls = [
            ("ue4ss_tasks_list", {"scope": "runtime"}),
            ("ue4ss_task_get", {"scope": "runtime", "taskId": "runtime-test-task-1"}),
            ("ue4ss_task_cancel", {"scope": "runtime", "taskId": "runtime-test-task-1"}),
            ("ue4ss_events_read", {"scope": "runtime", "cursor": 3, "limit": 25}),
        ]
        for name, arguments in calls:
            result = self.rpc("tools/call", {"name": name, "arguments": arguments})["result"]
            expected = self.runtime.tool_results[name]
            self.assertEqual(result["structuredContent"], expected)
            self.assertEqual(json.loads(result["content"][0]["text"]), expected)
            self.assertFalse(result["isError"])
        proxied = [params for method, params in self.runtime.calls if method == "tools/call"]
        self.assertEqual(
            proxied,
            [
                {"name": "ue4ss_tasks_list", "arguments": {}},
                {"name": "ue4ss_task_get", "arguments": {"taskId": "runtime-test-task-1"}},
                {"name": "ue4ss_task_cancel", "arguments": {"taskId": "runtime-test-task-1"}},
                {"name": "ue4ss_events_read", "arguments": {"cursor": 3, "limit": 25}},
            ],
        )

    def test_default_scope_stays_on_host_and_runtime_unavailable_does_not_fallback(self) -> None:
        waiting = self.call("ue4ss_scenario_run", {"scenario": {"name": "host-scope", "steps": [{"action": "wait", "seconds": 5}]}})
        cancelled = self.call("ue4ss_task_cancel", {"operationId": waiting["operationId"]})
        self.assertEqual(cancelled["operationId"], waiting["operationId"])
        self.assertTrue(cancelled["cancellationRequested"])
        listed = self.call("ue4ss_tasks_list", {})
        self.assertTrue(any(operation["operationId"] == waiting["operationId"] for operation in listed["currentState"]["operations"]))
        fetched = self.call("ue4ss_task_get", {"scope": "host", "operationId": waiting["operationId"]})
        self.assertEqual(fetched["currentState"]["operationId"], waiting["operationId"])
        events = self.call("ue4ss_events_read", {"cursor": 0, "limit": 1000})
        self.assertGreater(len(events["currentState"]["events"]), 0)
        self.assertFalse(any(method == "tools/call" for method, _ in self.runtime.calls))

        self.host.runtime.endpoint = None
        unavailable = self.rpc("tools/call", {"name": "ue4ss_tasks_list", "arguments": {"scope": "runtime"}})["result"]
        self.assertTrue(unavailable["isError"])
        self.assertEqual(unavailable["structuredContent"]["status"], "failed")
        self.assertEqual(unavailable["structuredContent"]["errors"][0]["code"], "lifecycle.runtime_unavailable")
        still_host = self.call("ue4ss_tasks_list", {})
        self.assertEqual(still_host["status"], "succeeded")
    def test_settings_profile_patch_preserves_layout_and_reports_override(self) -> None:
        base_path = self.game / "UE4SS-settings.ini"
        override_path = self.game / "UE4SS-settings.override.ini"
        base_raw = (
            b"\xef\xbb\xbf[MCP]\r\n"
            b"; keep this whole-line comment\r\n"
            b"Enabled = 0\r\n"
            b"Port = 8182\r\n"
            b"port = 7000\r\n"
            b"\r\n"
            b"[Other]\r\n"
            b"Untouched=exact\r\n"
        )
        base_path.write_bytes(base_raw)
        override_path.write_text("[MCP]\nPort = 9000\n", encoding="utf-8")

        profile = self.call(
            "ue4ss_settings_profile",
            {"root": "game", "path": base_path.name, "overridePath": override_path.name},
        )
        state = profile["currentState"]
        self.assertTrue(state["baseHasUtf8Bom"])
        self.assertEqual(state["baseSections"]["MCP"]["Port"], "8182")
        self.assertEqual(state["baseSections"]["MCP"]["port"], "7000")
        self.assertEqual(state["effectiveSections"]["MCP"]["Port"], "9000")
        self.assertEqual(state["overrideSources"][0]["sourcePath"], override_path.name)

        patched = self.call(
            "ue4ss_settings_patch",
            {
                "root": "game",
                "path": base_path.name,
                "overridePath": override_path.name,
                "expectedSha256": state["baseSha256"],
                "changes": [
                    {"section": "MCP", "key": "Enabled", "value": True},
                    {"section": "MCP", "key": "Port", "value": 8183},
                    {"section": "MCP", "key": "NewKey", "value": 7},
                    {"section": "Added", "key": "Value", "value": "text"},
                ],
            },
        )
        self.assertEqual(patched["status"], "succeeded")
        self.assertTrue(patched["restartRequired"])
        self.assertTrue(patched["currentState"]["verified"])
        changes = {(item["section"], item["key"]): item for item in patched["actualChanges"]}
        self.assertEqual(changes[("MCP", "Enabled")]["before"], "0")
        self.assertEqual(changes[("MCP", "Enabled")]["after"], "true")
        self.assertTrue(changes[("MCP", "Port")]["shadowedByOverride"])
        self.assertEqual(changes[("MCP", "Port")]["effectiveValue"], "9000")
        written = base_path.read_bytes()
        self.assertTrue(written.startswith(b"\xef\xbb\xbf"))
        self.assertIn(b"; keep this whole-line comment\r\nEnabled = true\r\n", written)
        self.assertIn(b"[Other]\r\nUntouched=exact\r\n", written)
        self.assertNotIn(b"\n", written.replace(b"\r\n", b""))
        self.assertEqual(override_path.read_text(encoding="utf-8"), "[MCP]\nPort = 9000\n")

        conflict = self.call(
            "ue4ss_settings_patch",
            {
                "root": "game",
                "path": base_path.name,
                "expectedSha256": state["baseSha256"],
                "changes": [{"section": "MCP", "key": "Enabled", "value": False}],
            },
        )
        self.assertEqual(conflict["status"], "rejected")
        self.assertEqual(conflict["errors"][0]["code"], "artifact.conflict")

        invalid_section = self.call(
            "ue4ss_settings_patch",
            {
                "root": "game",
                "path": base_path.name,
                "expectedSha256": patched["currentState"]["baseSha256"],
                "changes": [{"section": "MCP=Bad", "key": "Enabled", "value": True}],
            },
        )
        self.assertEqual(invalid_section["status"], "rejected")
        self.assertEqual(invalid_section["errors"][0]["code"], "settings.invalid_change")
        invalid_key = self.call(
            "ue4ss_settings_patch",
            {
                "root": "game",
                "path": base_path.name,
                "expectedSha256": patched["currentState"]["baseSha256"],
                "changes": [{"section": "MCP", "key": "Bad;Key", "value": True}],
            },
        )
        self.assertEqual(invalid_key["status"], "rejected")
        self.assertEqual(invalid_key["errors"][0]["code"], "settings.invalid_change")
        self.assertEqual(base_path.read_bytes(), written)


    def test_settings_parser_matches_native_whitespace_comments_and_insertion_order(self) -> None:
        exact_path = self.game / "exact-settings.ini"
        exact_path.write_text("[ MCP ]\nPort = 8182   \n\tTabbed=9\n", encoding="utf-8")
        exact_profile = self.call("ue4ss_settings_profile", {"root": "game", "path": exact_path.name})
        exact_state = exact_profile["currentState"]
        self.assertEqual(exact_state["baseSections"][" MCP "]["Port"], "8182   ")
        self.assertEqual(exact_state["baseSections"][" MCP "]["\tTabbed"], "9")

        exact_patch = self.call(
            "ue4ss_settings_patch",
            {
                "root": "game",
                "path": exact_path.name,
                "expectedSha256": exact_state["baseSha256"],
                "changes": [
                    {"section": " MCP ", "key": "Port", "value": 8183},
                    {"section": " MCP ", "key": "\tTabbed", "value": 10},
                ],
            },
        )
        self.assertEqual(exact_patch["status"], "succeeded")
        self.assertEqual(exact_path.read_text(encoding="utf-8"), "[ MCP ]\nPort = 8183\n\tTabbed=10\n")
        self.assertNotIn("MCP", exact_patch["currentState"]["baseSections"])

        mixed_path = self.game / "mixed-insertions.ini"
        mixed_path.write_text("[MCP]\nExisting=1\n", encoding="utf-8")
        mixed_profile = self.call("ue4ss_settings_profile", {"root": "game", "path": mixed_path.name})
        mixed_patch = self.call(
            "ue4ss_settings_patch",
            {
                "root": "game",
                "path": mixed_path.name,
                "expectedSha256": mixed_profile["currentState"]["baseSha256"],
                "changes": [
                    {"section": "New", "key": "Value", "value": 3},
                    {"section": "MCP", "key": "Added", "value": 2},
                ],
            },
        )
        self.assertEqual(mixed_patch["status"], "succeeded")
        self.assertEqual(mixed_patch["currentState"]["baseSections"]["MCP"]["Added"], "2")
        self.assertEqual(mixed_patch["currentState"]["baseSections"]["New"]["Value"], "3")
        self.assertLess(mixed_path.read_text(encoding="utf-8").index("Added = 2"), mixed_path.read_text(encoding="utf-8").index("[New]"))

        for name, text in (
            ("hash-comment.ini", "[MCP]\n# comment\nPort=8182\n"),
            ("header-comment.ini", "[MCP] ; comment\nPort=8182\n"),
        ):
            invalid_path = self.game / name
            invalid_path.write_text(text, encoding="utf-8")
            invalid = self.call("ue4ss_settings_profile", {"root": "game", "path": invalid_path.name})
            self.assertEqual(invalid["status"], "failed")
            self.assertEqual(invalid["errors"][0]["code"], "settings.parse_error")

    def test_settings_hash_guard_is_serialized_across_managers(self) -> None:
        path = self.game / "UE4SS-settings.ini"
        path.write_text("[MCP]\nPort = 8182\n", encoding="utf-8")
        profile = self.host.settings.profile("game", path.name)
        expected_hash = profile["currentState"]["baseSha256"]
        other_workspace = Workspace({"game": self.game}, Path(self.temporary.name) / "other-state")
        other_manager = SettingsManager(other_workspace)
        entered_replace = threading.Event()
        release_replace = threading.Event()
        outcomes: dict[str, object] = {}
        original_replace = self.host.settings._atomic_replace

        def delayed_replace(target: Path, data: bytes) -> None:
            entered_replace.set()
            self.assertTrue(release_replace.wait(timeout=5))
            original_replace(target, data)

        def first_patch() -> None:
            outcomes["first"] = self.host.settings.patch(
                "game",
                path.name,
                [{"section": "MCP", "key": "Port", "value": 8183}],
                expected_hash,
            )

        def second_patch() -> None:
            try:
                outcomes["second"] = other_manager.patch(
                    "game",
                    path.name,
                    [{"section": "MCP", "key": "Port", "value": 8184}],
                    expected_hash,
                )
            except ToolError as error:
                outcomes["secondError"] = error

        with mock.patch.object(self.host.settings, "_atomic_replace", side_effect=delayed_replace):
            first = threading.Thread(target=first_patch)
            second = threading.Thread(target=second_patch)
            first.start()
            self.assertTrue(entered_replace.wait(timeout=5))
            second.start()
            time.sleep(0.1)
            self.assertTrue(second.is_alive())
            release_replace.set()
            first.join(timeout=5)
            second.join(timeout=5)

        self.assertFalse(first.is_alive())
        self.assertFalse(second.is_alive())
        self.assertIn("first", outcomes)
        self.assertNotIn("second", outcomes)
        self.assertEqual(outcomes["secondError"].code, "artifact.conflict")
        self.assertEqual(path.read_text(encoding="utf-8"), "[MCP]\nPort = 8183\n")

    def test_memory_capability_is_stable_off_windows(self) -> None:
        profile = self.call("ue4ss_memory_profile", {})
        self.assertEqual(profile["status"], "succeeded")
        self.assertEqual(profile["currentState"]["available"], sys.platform == "win32")
        if sys.platform != "win32":
            unavailable = self.call(
                "ue4ss_memory_read",
                {"processId": os.getpid(), "address": 0, "size": 1},
            )
            self.assertEqual(unavailable["status"], "failed")
            self.assertEqual(unavailable["errors"][0]["code"], "capability.unavailable")

    def test_memory_indeterminate_writes_record_actual_changes(self) -> None:
        cases = [
            (
                ToolError(
                    "memory.partial_write",
                    "partial write",
                    stage="memory.write",
                    context={"processId": os.getpid(), "address": 4096, "requestedBytes": 4, "bytesWritten": 2, "winError": 299},
                    status="indeterminate",
                ),
                "partially_written",
                2,
            ),
            (
                ToolError(
                    "memory.write_verification_failed",
                    "verification mismatch",
                    stage="memory.write.verify",
                    context={"processId": os.getpid(), "address": 4096, "bytesWritten": 4, "requestedSha256": "a", "verifiedSha256": "b"},
                    status="indeterminate",
                ),
                "written_unverified",
                4,
            ),
        ]
        for error, action, bytes_written in cases:
            with self.subTest(code=error.code), mock.patch.object(self.host.memory, "write", side_effect=error):
                result = self.call(
                    "ue4ss_memory_write",
                    {"processId": os.getpid(), "address": 4096, "contentBase64": "AQIDBA=="},
                )
            self.assertEqual(result["status"], "indeterminate")
            self.assertEqual(result["errors"][0]["code"], error.code)
            self.assertEqual(result["actualChanges"][0]["action"], action)
            self.assertEqual(result["actualChanges"][0]["bytesWritten"], bytes_written)

    def test_process_lifecycle_is_scoped_explicit_and_bounded(self) -> None:
        profile = self.call("ue4ss_process_profile", {})
        self.assertTrue(profile["currentState"]["available"])
        escaped = self.call(
            "ue4ss_process_start",
            {"root": "python", "executable": f"../{Path(sys.executable).name}", "arguments": []},
        )
        self.assertEqual(escaped["status"], "rejected")
        self.assertEqual(escaped["errors"][0]["code"], "workspace.path_outside_root")

        started = self.call(
            "ue4ss_process_start",
            {
                "root": "python",
                "executable": Path(sys.executable).name,
                "arguments": ["-c", "import time; time.sleep(60)"],
                "workingDirectory": ".",
            },
        )
        self.assertEqual(started["status"], "succeeded")
        pid = started["currentState"]["pid"]
        self.started_process_ids.append(pid)
        self.assertTrue(started["currentState"]["running"])

        discovered = self.call(
            "ue4ss_process_discover",
            {"root": "python", "executableName": started["currentState"]["executableName"], "limit": 1000},
        )
        by_pid = {process["pid"]: process for process in discovered["currentState"]["processes"]}
        self.assertIn(pid, by_pid)
        self.assertTrue(by_pid[pid]["running"])
        self.assertTrue(by_pid[pid]["hostStarted"])
        self.assertEqual(by_pid[pid]["rootMatches"][0]["name"], "python")

        restarted = self.call(
            "ue4ss_process_restart",
            {"processId": pid, "force": True, "timeoutSeconds": 0.2},
        )
        self.assertEqual(restarted["status"], "succeeded")
        replacement_pid = restarted["currentState"]["process"]["pid"]
        self.started_process_ids.append(replacement_pid)
        stopped = self.call(
            "ue4ss_process_stop",
            {"processId": replacement_pid, "force": True, "timeoutSeconds": 0.2},
        )
        self.assertEqual(stopped["status"], "succeeded")
        self.assertFalse(stopped["currentState"]["running"])

        missing = self.call("ue4ss_process_stop", {"processId": 2**31 - 1, "force": True, "timeoutSeconds": 0.1})
        self.assertEqual(missing["status"], "failed")
        self.assertEqual(missing["errors"][0]["code"], "process.not_found")

    @unittest.skipUnless(sys.platform == "win32", "Windows process memory APIs are required")
    def test_memory_read_write_regions_scan_bounds_and_handle_lifetime(self) -> None:
        profile = self.call("ue4ss_memory_profile", {})
        self.assertEqual(profile["status"], "succeeded")
        self.assertTrue(profile["currentState"]["available"])
        self.assertEqual(profile["currentState"]["limits"]["maxTransferBytes"], 1024 * 1024)

        original = b"\x13\x37UE4SS_MEMORY_TEST\xde\xad\xbe\xef"
        buffer = ctypes.create_string_buffer(original)
        address = ctypes.addressof(buffer)
        process_id = os.getpid()

        regions = self.call(
            "ue4ss_memory_regions",
            {"processId": process_id, "startAddress": address, "size": len(buffer.raw), "maxRegions": 10},
        )
        self.assertEqual(regions["status"], "succeeded")
        self.assertTrue(regions["currentState"]["regions"][0]["readable"])
        self.assertLessEqual(regions["currentState"]["regions"][0]["rangeStart"], address)

        read = self.call(
            "ue4ss_memory_read",
            {"processId": process_id, "address": address, "size": len(original)},
        )
        self.assertEqual(read["status"], "succeeded")
        self.assertEqual(base64.b64decode(read["currentState"]["contentBase64"]), original)

        replacement = b"\x99\x88"
        written = self.call(
            "ue4ss_memory_write",
            {
                "processId": process_id,
                "address": address + 2,
                "contentBase64": base64.b64encode(replacement).decode("ascii"),
            },
        )
        self.assertEqual(written["status"], "succeeded")
        self.assertTrue(written["currentState"]["verifiedEqual"])
        self.assertEqual(written["currentState"]["requestedSha256"], written["currentState"]["verifiedSha256"])
        self.assertEqual(buffer.raw[2:4], replacement)

        accepted = self.call(
            "ue4ss_memory_pattern_scan",
            {
                "processId": process_id,
                "startAddress": address,
                "size": len(buffer.raw),
                "pattern": "13 37 99 ?? 34 53 53",
                "maxMatches": 10,
            },
        )
        scanned = self.wait_operation(accepted["operationId"])
        self.assertEqual(scanned["status"], "succeeded")
        self.assertEqual(scanned["currentState"]["matches"], [address])
        self.assertEqual(scanned["progress"], 1.0)

        oversized = self.call(
            "ue4ss_memory_read",
            {"processId": process_id, "address": address, "size": 1024 * 1024 + 1},
        )
        self.assertEqual(oversized["status"], "failed")
        self.assertEqual(oversized["errors"][0]["code"], "transport.invalid_arguments")
        invalid_region = self.call(
            "ue4ss_memory_read",
            {"processId": process_id, "address": 0, "size": 1},
        )
        self.assertEqual(invalid_region["status"], "failed")
        self.assertEqual(invalid_region["errors"][0]["code"], "memory.read_failed")
        invalid_pattern = self.call(
            "ue4ss_memory_pattern_scan",
            {"processId": process_id, "startAddress": address, "size": len(buffer.raw), "pattern": "not-hex"},
        )
        invalid_pattern = self.wait_operation(invalid_pattern["operationId"])
        self.assertEqual(invalid_pattern["status"], "failed")
        self.assertEqual(invalid_pattern["errors"][0]["code"], "memory.invalid_pattern")

        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.GetCurrentProcess.restype = wintypes.HANDLE
        kernel32.GetProcessHandleCount.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        kernel32.GetProcessHandleCount.restype = wintypes.BOOL
        handle_count = wintypes.DWORD()
        self.assertTrue(kernel32.GetProcessHandleCount(kernel32.GetCurrentProcess(), ctypes.byref(handle_count)))
        before = handle_count.value
        for _ in range(32):
            self.host.memory.read(process_id, address, 1)
        self.assertLessEqual(handle_count.value, before)

    def test_scoped_files_backup_restore_and_conflict(self) -> None:
        written = self.call("ue4ss_files_write", {"root": "source", "path": "nested/value.txt", "content": "before"})
        self.assertEqual(written["status"], "succeeded")
        file_hash = written["currentState"]["sha256"]
        read = self.call("ue4ss_files_read", {"root": "source", "path": "nested/value.txt"})
        self.assertEqual(read["currentState"]["content"], "before")
        backup = self.call("ue4ss_files_backup", {"name": "baseline", "items": [{"root": "source", "path": "nested/value.txt"}]})
        self.assertEqual(backup["status"], "succeeded")
        changed = self.call("ue4ss_files_write", {"root": "source", "path": "nested/value.txt", "content": "after", "expectedSha256": file_hash})
        self.assertEqual(changed["status"], "succeeded")
        restore = self.call("ue4ss_files_restore", {"name": "baseline"})
        self.assertEqual(restore["status"], "partial")
        self.assertEqual(restore["currentState"]["conflicts"][0]["path"], "nested/value.txt")
        escaped = self.call("ue4ss_files_read", {"root": "source", "path": "../outside.txt"})
        self.assertEqual(escaped["status"], "rejected")
        invalid_base64 = self.call("ue4ss_files_write", {"root": "source", "path": "bad.bin", "content": "%%%", "encoding": "base64"})
        self.assertEqual(invalid_base64["errors"][0]["code"], "artifact.invalid_base64")
        (self.game / "UE4SS.log").write_text("runtime log", encoding="utf-8")
        crashes = self.call(
            "ue4ss_crash_artifacts",
            {"locations": [{"root": "game", "path": "UE4SS.log"}, {"root": "game", "path": ""}]},
        )
        self.assertEqual(len(crashes["currentState"]["artifacts"]), 1)
        self.assertFalse(crashes["currentState"]["truncated"])

    def test_expected_hash_write_is_serialized_per_scoped_path(self) -> None:
        initial = self.host.workspace.write("source", "concurrent.txt", "initial")
        expected_hash = initial["currentState"]["sha256"]
        barrier = threading.Barrier(3)
        successes: list[dict[str, object]] = []
        failures: list[ToolError] = []
        real_replace = os.replace

        def delayed_replace(source: str | bytes | os.PathLike[str] | os.PathLike[bytes], destination: str | bytes | os.PathLike[str] | os.PathLike[bytes]) -> None:
            time.sleep(0.05)
            real_replace(source, destination)

        def overwrite(content: str) -> None:
            barrier.wait()
            try:
                successes.append(
                    self.host.workspace.write(
                        "source",
                        "concurrent.txt",
                        content,
                        expected_sha256=expected_hash,
                    )
                )
            except ToolError as error:
                failures.append(error)

        threads = [threading.Thread(target=overwrite, args=(content,)) for content in ("first", "second")]
        with mock.patch("tools.mcp_host.workspace.os.replace", side_effect=delayed_replace):
            for thread in threads:
                thread.start()
            barrier.wait()
            for thread in threads:
                thread.join(timeout=2)

        self.assertTrue(all(not thread.is_alive() for thread in threads))
        self.assertEqual(len(successes), 1)
        self.assertEqual([error.code for error in failures], ["artifact.conflict"])
        self.assertIn((self.source / "concurrent.txt").read_text(encoding="utf-8"), {"first", "second"})

    def test_expected_hash_delete_is_serialized_per_scoped_path(self) -> None:
        initial = self.host.workspace.write("source", "concurrent-delete.txt", "initial")
        expected_hash = initial["currentState"]["sha256"]
        target = self.source / "concurrent-delete.txt"
        barrier = threading.Barrier(3)
        successes: list[dict[str, object]] = []
        failures: list[ToolError] = []
        real_unlink = Path.unlink

        def delayed_unlink(path: Path, *args: object, **kwargs: object) -> None:
            if path == target:
                time.sleep(0.05)
            real_unlink(path, *args, **kwargs)

        def delete() -> None:
            barrier.wait()
            try:
                successes.append(
                    self.host.workspace.delete(
                        "source",
                        "concurrent-delete.txt",
                        expected_sha256=expected_hash,
                    )
                )
            except ToolError as error:
                failures.append(error)

        threads = [threading.Thread(target=delete) for _ in range(2)]
        with mock.patch.object(Path, "unlink", autospec=True, side_effect=delayed_unlink):
            for thread in threads:
                thread.start()
            barrier.wait()
            for thread in threads:
                thread.join(timeout=2)

        self.assertTrue(all(not thread.is_alive() for thread in threads))
        self.assertEqual(len(successes), 1)
        self.assertEqual([error.code for error in failures], ["artifact.conflict"])
        self.assertFalse(target.exists())

    def test_restore_rejects_backup_name_traversal(self) -> None:
        outside_archive = self.host.workspace.backup_directory.parent / "outside.zip"
        with self.assertRaises(ToolError) as rejected:
            self.host.workspace.restore("../outside", overwrite=True)
        self.assertEqual(rejected.exception.code, "artifact.invalid_backup_name")
        self.assertEqual(rejected.exception.stage, "files.restore")
        self.assertFalse(outside_archive.exists())

    def test_mod_overwrite_commit_failure_restores_old_installation(self) -> None:
        source = self.source / "replacement" / "AtomicMod" / "Scripts"
        source.mkdir(parents=True)
        (source / "main.lua").write_text("new", encoding="utf-8")
        destination = self.game / "Mods" / "AtomicMod"
        (destination / "Scripts").mkdir(parents=True)
        (destination / "Scripts" / "main.lua").write_text("old", encoding="utf-8")
        (destination / "old-only.txt").write_text("preserve", encoding="utf-8")
        real_replace = Path.replace

        def fail_staged_commit(path: Path, target: str | os.PathLike[str]) -> Path:
            if path.name == "staged":
                raise PermissionError("destination is locked")
            return real_replace(path, target)

        with mock.patch.object(Path, "replace", autospec=True, side_effect=fail_staged_commit):
            with self.assertRaises(ToolError) as failed:
                self.host.mods.install("source", "replacement/AtomicMod", "game", "Mods", "AtomicMod", overwrite=True)

        self.assertEqual(failed.exception.code, "mod.install_commit_failed")
        self.assertEqual((destination / "Scripts" / "main.lua").read_text(encoding="utf-8"), "old")
        self.assertEqual((destination / "old-only.txt").read_text(encoding="utf-8"), "preserve")

    def test_mod_overwrite_locked_old_install_is_not_deleted(self) -> None:
        source = self.source / "replacement" / "LockedMod" / "Scripts"
        source.mkdir(parents=True)
        (source / "main.lua").write_text("new", encoding="utf-8")
        destination = self.game / "Mods" / "LockedMod"
        (destination / "Scripts").mkdir(parents=True)
        installed_script = destination / "Scripts" / "main.lua"
        installed_script.write_text("old", encoding="utf-8")
        os.chmod(installed_script, 0o444)
        real_replace = Path.replace

        def fail_old_tree_rename(path: Path, target: str | os.PathLike[str]) -> Path:
            if path.name == "LockedMod":
                raise PermissionError("installed file is loaded")
            return real_replace(path, target)

        with mock.patch.object(Path, "replace", autospec=True, side_effect=fail_old_tree_rename):
            with self.assertRaises(ToolError) as failed:
                self.host.mods.install("source", "replacement/LockedMod", "game", "Mods", "LockedMod", overwrite=True)

        self.assertEqual(failed.exception.code, "mod.install_commit_failed")
        self.assertTrue(destination.is_dir())
        self.assertEqual(installed_script.read_text(encoding="utf-8"), "old")

    def test_mod_overwrite_backup_cleanup_failure_keeps_verified_install_and_backup(self) -> None:
        from tools.mcp_host import mods as mods_module

        source = self.source / "replacement" / "CleanupMod" / "Scripts"
        source.mkdir(parents=True)
        (source / "main.lua").write_text("new", encoding="utf-8")
        destination = self.game / "Mods" / "CleanupMod"
        (destination / "Scripts").mkdir(parents=True)
        (destination / "Scripts" / "main.lua").write_text("old", encoding="utf-8")
        real_remove_tree = mods_module.remove_tree

        def fail_backup_cleanup(path: Path) -> None:
            if path.name == "backup":
                raise PermissionError("read-only file is in use")
            real_remove_tree(path)

        with mock.patch("tools.mcp_host.mods.remove_tree", side_effect=fail_backup_cleanup):
            result = self.host.mods.install("source", "replacement/CleanupMod", "game", "Mods", "CleanupMod", overwrite=True)

        backup = Path(result["currentState"]["backupPath"])
        self.assertEqual(result["status"], "partial")
        self.assertTrue(result["currentState"]["cleanupPending"])
        self.assertEqual(result["errors"][0]["code"], "mod.install_backup_cleanup_failed")
        self.assertEqual((destination / "Scripts" / "main.lua").read_text(encoding="utf-8"), "new")
        self.assertEqual((backup / "Scripts" / "main.lua").read_text(encoding="utf-8"), "old")

    def test_lua_mod_lifecycle_is_idempotent_and_hash_verified(self) -> None:
        created = self.call("ue4ss_mod_create", {"type": "lua", "root": "source", "directory": "mods", "name": "HelloMcp", "entryScript": "print('hello')\n"})
        self.assertEqual(created["status"], "succeeded")
        self.assertTrue((self.source / "mods" / "HelloMcp" / "Scripts" / "main.lua").is_file())
        (self.source / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.22)\n", encoding="utf-8")
        cpp = self.call(
            "ue4ss_mod_create",
            {
                "type": "cpp",
                "root": "source",
                "directory": "mods",
                "name": "CppMcp",
                "ue4ssSource": self.source.as_posix(),
            },
        )
        self.assertEqual(cpp["status"], "succeeded")
        cmake = (self.source / "mods" / "CppMcp" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("EXCLUDE_FROM_ALL", cmake)
        self.assertIn("OUTPUT_NAME main", cmake)
        self.assertIn("RUNTIME DESTINATION dlls", cmake)
        installed = self.call("ue4ss_mod_install", {"sourceRoot": "source", "sourcePath": "mods/HelloMcp", "destinationRoot": "game", "modsPath": "Mods", "name": "HelloMcp"})
        self.assertTrue(installed["restartRequired"])
        repeated = self.call("ue4ss_mod_install", {"sourceRoot": "source", "sourcePath": "mods/HelloMcp", "destinationRoot": "game", "modsPath": "Mods", "name": "HelloMcp"})
        self.assertEqual(repeated["actualChanges"], [])
        (self.game / "Mods" / "mods.txt").write_text("Keybinds : 1\n", encoding="utf-8")
        enabled = self.call("ue4ss_mod_set_enabled", {"root": "game", "modsPath": "Mods", "name": "HelloMcp", "enabled": True})
        self.assertEqual(enabled["currentState"]["enabled"], True)
        self.assertEqual(enabled["currentState"]["manifest"], "Mods/mods.txt")
        self.assertIn("HelloMcp : 1", (self.game / "Mods" / "mods.txt").read_text(encoding="utf-8"))
        expected = installed["currentState"]["artifacts"]
        (self.game / "Mods" / "HelloMcp" / "Scripts" / "main.lua").write_text("changed", encoding="utf-8")
        conflict = self.call("ue4ss_mod_remove", {"root": "game", "modsPath": "Mods", "name": "HelloMcp", "expectedManifest": expected})
        self.assertEqual(conflict["errors"][0]["code"], "mod.remove_conflict")
        self.assertTrue((self.game / "Mods" / "HelloMcp").is_dir())
        removed = self.call("ue4ss_mod_remove", {"root": "game", "modsPath": "Mods", "name": "HelloMcp"})
        self.assertEqual(removed["currentState"]["installed"], False)

    def test_mod_enable_creates_runtime_manifest_and_updates_duplicate_entries(self) -> None:
        mods = self.game / "Mods"
        mods.mkdir(parents=True, exist_ok=True)
        created = self.call("ue4ss_mod_set_enabled", {"root": "game", "modsPath": "Mods", "name": "HelloMcp", "enabled": True})
        self.assertEqual(created["currentState"]["manifest"], "Mods/mods.txt")
        self.assertFalse((mods / "mods.json").exists())

        (mods / "mods.txt").write_text("HelloMcp : 1\nHelloMcp : 1\n", encoding="utf-8")
        disabled = self.call("ue4ss_mod_set_enabled", {"root": "game", "modsPath": "Mods", "name": "HelloMcp", "enabled": False})
        self.assertEqual(disabled["currentState"]["enabled"], False)
        self.assertEqual((mods / "mods.txt").read_text(encoding="utf-8").count("HelloMcp : 0"), 2)

    def test_mod_disable_rejects_enabled_marker_override(self) -> None:
        mod = self.game / "Mods" / "HelloMcp"
        mod.mkdir(parents=True, exist_ok=True)
        (self.game / "Mods" / "mods.txt").write_text("HelloMcp : 1\n", encoding="utf-8")
        (mod / "enabled.txt").write_text("", encoding="utf-8")

        result = self.call("ue4ss_mod_set_enabled", {"root": "game", "modsPath": "Mods", "name": "HelloMcp", "enabled": False})

        self.assertEqual(result["errors"][0]["code"], "mod.enabled_marker_conflict")
        self.assertEqual((self.game / "Mods" / "mods.txt").read_text(encoding="utf-8"), "HelloMcp : 1\n")

    def test_native_debug_dap_round_trip(self) -> None:
        profile = self.call("ue4ss_native_debug_profile", {})
        self.assertTrue(profile["currentState"]["available"])
        attached = self.call(
            "ue4ss_native_debug_attach",
            {"processId": 1234, "adapterArguments": [str(self.adapter)], "attachArguments": {}},
        )
        session_id = attached["currentState"]["sessionId"]
        events = self.call("ue4ss_events_read", {"cursor": 0, "limit": 1000})["currentState"]["events"]
        self.assertTrue(
            any(event["kind"] == "debug.dap_event" and event["data"]["event"] == "output" for event in events)
        )
        operations = self.call("ue4ss_tasks_list", {})["currentState"]["operations"]
        self.assertTrue(any(operation["target"] == "ue4ss_native_debug_output" for operation in operations))
        breakpoints = self.call(
            "ue4ss_native_debug_breakpoints_set",
            {"sessionId": session_id, "root": "source", "path": "debug.cpp", "breakpoints": [{"line": 1}]},
        )
        self.assertTrue(breakpoints["currentState"]["breakpoints"][0]["verified"])
        stack = self.call("ue4ss_native_debug_stack", {"sessionId": session_id, "threadId": 1})
        self.assertEqual(stack["currentState"]["stackTrace"]["stackFrames"][0]["name"], "Tick")
        scopes = self.call("ue4ss_native_debug_variables", {"sessionId": session_id, "frameId": 10})
        self.assertEqual(scopes["currentState"]["scopes"][0]["name"], "Locals")
        variables = self.call("ue4ss_native_debug_variables", {"sessionId": session_id, "variablesReference": 20})
        self.assertEqual(variables["currentState"]["variables"][0]["name"], "Counter")
        evaluated = self.call("ue4ss_native_debug_evaluate", {"sessionId": session_id, "expression": "Counter", "frameId": 10})
        self.assertEqual(evaluated["currentState"]["result"], "1")
        continued = self.call("ue4ss_native_debug_control", {"sessionId": session_id, "command": "continue", "threadId": 1})
        self.assertEqual(continued["currentState"]["command"], "continue")
        detached = self.call("ue4ss_native_debug_detach", {"sessionId": session_id})
        self.assertFalse(detached["currentState"]["connected"])

    def test_native_debug_variables_default_is_bounded_and_ignored_paging_is_truncated(self) -> None:
        attached = self.call(
            "ue4ss_native_debug_attach",
            {"processId": 1234, "adapterArguments": [str(self.adapter)], "attachArguments": {}},
        )
        session_id = attached["currentState"]["sessionId"]
        variables = self.call(
            "ue4ss_native_debug_variables",
            {"sessionId": session_id, "variablesReference": 99},
        )
        state = variables["currentState"]
        self.assertEqual(state["requestedCount"], 100)
        self.assertEqual(len(state["variables"]), 100)
        self.assertEqual(state["returnedCount"], 100)
        self.assertEqual(state["returned"], 100)
        self.assertEqual(state["adapterReturnedCount"], 150)
        self.assertTrue(state["truncated"])
        self.assertTrue(state["adapterIgnoredPaging"])
        self.assertEqual(state["nextStart"], 100)
        evidence_variables = variables["evidence"][0]["response"]["body"]["variables"]
        self.assertEqual(len(evidence_variables), 100)
        self.call("ue4ss_native_debug_detach", {"sessionId": session_id})

    def test_native_debug_lease_renews_then_resumes_and_detaches_on_expiry(self) -> None:
        command_log = Path(self.temporary.name) / "dap_commands.log"
        attached = self.call(
            "ue4ss_native_debug_attach",
            {
                "processId": 1234,
                "adapterArguments": [str(self.adapter), str(command_log)],
                "attachArguments": {},
                "leaseSeconds": 1,
            },
        )
        session_id = attached["currentState"]["sessionId"]
        first_activity = attached["currentState"]["lastActivityAt"]
        time.sleep(0.6)
        self.call("ue4ss_native_debug_stack", {"sessionId": session_id})
        sessions = self.call("ue4ss_native_debug_sessions", {})["currentState"]["sessions"]
        session = next(item for item in sessions if item["sessionId"] == session_id)
        self.assertGreater(session["lastActivityAt"], first_activity)
        self.assertEqual(session["renewalPolicy"], "eachSessionOperation")
        time.sleep(0.6)
        sessions = self.call("ue4ss_native_debug_sessions", {})["currentState"]["sessions"]
        self.assertTrue(any(item["sessionId"] == session_id for item in sessions))
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            sessions = self.call("ue4ss_native_debug_sessions", {})["currentState"]["sessions"]
            if not any(item["sessionId"] == session_id for item in sessions):
                break
            time.sleep(0.05)
        else:
            self.fail("expired DAP session was not detached")
        command_deadline = time.monotonic() + 3
        commands: list[str] = []
        while time.monotonic() < command_deadline:
            if command_log.exists():
                commands = command_log.read_text(encoding="utf-8").splitlines()
                if "disconnect" in commands:
                    break
            time.sleep(0.05)
        self.assertIn("continue", commands)
        self.assertIn("disconnect", commands)
        self.assertLess(commands.index("continue"), commands.index("disconnect"))
        event_records = self.call("ue4ss_events_read", {"cursor": 0, "limit": 1000})["currentState"]["events"]
        event_kinds = {event["kind"] for event in event_records}
        self.assertIn("debug.lease_expired", event_kinds)
        self.assertIn("debug.target_resumed", event_kinds)
        operations = self.call("ue4ss_tasks_list", {"cursor": 0, "limit": 1000})["currentState"]["operations"]
        expiry_summary = next(operation for operation in operations if operation["target"] == "ue4ss_native_debug_lease_expired")
        expiry_operation = self.call("ue4ss_task_get", {"operationId": expiry_summary["operationId"]})["currentState"]
        self.assertEqual(expiry_operation["evidence"][0]["kind"], "dap_lease_expiry")
        self.assertTrue(expiry_operation["evidence"][0]["resume"]["resumed"])

    def test_native_debug_configuration_done_respects_capability_and_is_sent_once(self) -> None:
        command_log = Path(self.temporary.name) / "dap_configuration_commands.log"
        attached = self.call(
            "ue4ss_native_debug_attach",
            {
                "processId": 1234,
                "adapterArguments": [str(self.adapter), str(command_log)],
                "attachArguments": {},
            },
        )
        session_id = attached["currentState"]["sessionId"]
        first = self.call(
            "ue4ss_native_debug_breakpoints_set",
            {"sessionId": session_id, "root": "source", "path": "debug.cpp", "breakpoints": [{"line": 1}]},
        )
        self.assertTrue(first["currentState"]["configurationDone"]["sent"])
        second = self.call(
            "ue4ss_native_debug_breakpoints_set",
            {"sessionId": session_id, "root": "source", "path": "debug.cpp", "breakpoints": [{"line": 1}]},
        )
        self.assertTrue(second["currentState"]["configurationDone"]["alreadyConfigured"])
        repeated = self.call(
            "ue4ss_native_debug_control",
            {"sessionId": session_id, "command": "configurationDone"},
        )
        self.assertEqual(repeated["actualChanges"], [])
        self.call("ue4ss_native_debug_detach", {"sessionId": session_id})
        commands = command_log.read_text(encoding="utf-8").splitlines()
        self.assertEqual(commands.count("configurationDone"), 1)
        unsupported = self.call(
            "ue4ss_native_debug_attach",
            {
                "processId": 1234,
                "adapterArguments": [str(self.adapter), "--no-config"],
                "attachArguments": {},
            },
        )
        unsupported_session_id = unsupported["currentState"]["sessionId"]
        unsupported_breakpoints = self.call(
            "ue4ss_native_debug_breakpoints_set",
            {"sessionId": unsupported_session_id, "root": "source", "path": "debug.cpp", "breakpoints": [{"line": 1}]},
        )
        configuration = unsupported_breakpoints["currentState"]["configurationDone"]
        self.assertFalse(configuration["supported"])
        self.assertFalse(configuration["sent"])
        self.call("ue4ss_native_debug_detach", {"sessionId": unsupported_session_id})

    def test_task_list_pages_summaries_and_task_get_returns_full_state(self) -> None:
        created = [
            self.host.operations.result(
                f"task-page-{index}",
                state={"large": "x" * 10_000},
                changes=[{"kind": "change", "index": index}],
                evidence=[{"kind": "evidence", "index": index}],
            )
            for index in range(3)
        ]
        first_page = self.call("ue4ss_tasks_list", {"cursor": 0, "limit": 2})["currentState"]
        self.assertEqual(
            [operation["operationId"] for operation in first_page["operations"]],
            [created[2]["operationId"], created[1]["operationId"]],
        )
        self.assertFalse(first_page["endOfList"])
        for summary in first_page["operations"]:
            self.assertNotIn("currentState", summary)
            self.assertNotIn("actualChanges", summary)
            self.assertNotIn("evidence", summary)
            self.assertNotIn("errors", summary)

        second_page = self.call(
            "ue4ss_tasks_list",
            {"cursor": first_page["nextCursor"], "limit": 2},
        )["currentState"]
        self.assertEqual([operation["operationId"] for operation in second_page["operations"]], [created[0]["operationId"]])
        self.assertTrue(second_page["endOfList"])
        full = self.call("ue4ss_task_get", {"operationId": created[2]["operationId"]})["currentState"]
        self.assertEqual(full["currentState"], {"large": "x" * 10_000})
        self.assertEqual(full["actualChanges"], [{"kind": "change", "index": 2}])
        self.assertEqual(full["evidence"], [{"kind": "evidence", "index": 2}])


    def test_mod_create_schema_conditionally_requires_cpp_source(self) -> None:
        tools = self.rpc("tools/list")["result"]["tools"]
        schema = next(tool for tool in tools if tool["name"] == "ue4ss_mod_create")["inputSchema"]
        self.assertEqual(schema["if"], {"properties": {"type": {"const": "cpp"}}, "required": ["type"]})
        self.assertEqual(schema["then"], {"required": ["ue4ssSource"]})
        self.assertEqual(schema["properties"]["ue4ssSource"]["minLength"], 1)

    def test_editor_profile_degrades_until_version_evidence_matches(self) -> None:
        editor = self.source / "UE_5.3" / "Engine" / "Binaries" / "Win64" / "UnrealEditor-Cmd.exe"
        editor.parent.mkdir(parents=True)
        editor.write_bytes(b"fake editor")
        self.host.toolchains.configured["unrealEditor"] = str(editor)

        unverified = self.call("ue4ss_editor_profile", {})["currentState"]
        self.assertTrue(unverified["configured"])
        self.assertTrue(unverified["degraded"])
        self.assertTrue(unverified["unverified"])
        self.assertIsNone(unverified["compatible"])
        self.assertNotIn("runScript", unverified["supportedOperations"])
        self.assertIn("runScript", unverified["degradedOperations"])

        project = self.source / "ProfileTest.uproject"
        project.write_text(json.dumps({"EngineAssociation": "5.3"}), encoding="utf-8")
        verified = self.call(
            "ue4ss_editor_profile",
            {"root": "source", "projectPath": project.name, "targetVersion": "5.3.2"},
        )["currentState"]
        self.assertEqual(verified["compatibility"], "verified")
        self.assertTrue(verified["compatible"])
        self.assertFalse(verified["degraded"])
        self.assertFalse(verified["unverified"])
        self.assertIn("runScript", verified["supportedOperations"])

    def test_uat_package_requires_and_returns_hashed_artifacts(self) -> None:
        project = self.source / "PackageTest.uproject"
        project.write_text("{}", encoding="utf-8")
        output = self.source / "PackageOutput"
        output.mkdir()
        run_uat = self.source / "RunUAT.bat"
        run_uat.write_text("@exit /b 0\n", encoding="utf-8")
        self.host.toolchains.configured["runUAT"] = str(run_uat)
        process = mock.Mock(return_code=0)
        process.as_dict.return_value = {
            "command": [str(run_uat), "BuildPlugin"],
            "cwd": self.source.as_posix(),
            "returnCode": 0,
            "output": [],
            "diagnostics": [],
            "cancelled": False,
        }

        with mock.patch.object(self.host.toolchains.runner, "run", return_value=process):
            empty = self.call(
                "ue4ss_editor_package",
                {
                    "root": "source",
                    "projectPath": project.name,
                    "command": "BuildPlugin",
                    "arguments": [f"-Package={output.name}"],
                },
            )
            empty_result = self.wait_operation(empty["operationId"])
        self.assertEqual(empty_result["status"], "failed")
        self.assertEqual(empty_result["errors"][0]["code"], "editor.package_missing_artifacts")

        artifact = output / "ProfileTest.uplugin"
        artifact.write_text('{"FileVersion": 3}', encoding="utf-8")
        with mock.patch.object(self.host.toolchains.runner, "run", return_value=process):
            accepted = self.call(
                "ue4ss_editor_package",
                {
                    "root": "source",
                    "projectPath": project.name,
                    "command": "BuildPlugin",
                    "arguments": [f"-Package={output.name}"],
                },
            )
            completed = self.wait_operation(accepted["operationId"])
        state = completed["currentState"]
        self.assertEqual(completed["status"], "succeeded")
        self.assertTrue(state["completed"])
        self.assertEqual(state["artifactPathSource"], "arguments")
        self.assertFalse(state["artifactsTruncated"])
        self.assertEqual(len(state["artifacts"]), 1)
        self.assertEqual(len(state["artifacts"][0]["sha256"]), 64)
        self.assertEqual(completed["actualChanges"][0]["action"], "verified")

    def test_uat_package_reports_truncated_artifact_manifest(self) -> None:
        project = self.source / "TruncatedPackage.uproject"
        project.write_text("{}", encoding="utf-8")
        run_uat = self.source / "RunUAT.bat"
        run_uat.write_text("@exit /b 0\n", encoding="utf-8")
        self.host.toolchains.configured["runUAT"] = str(run_uat)
        process = mock.Mock(return_code=0)
        process.as_dict.return_value = {
            "command": [str(run_uat), "BuildCookRun"],
            "cwd": self.source.as_posix(),
            "returnCode": 0,
            "output": [],
            "diagnostics": [],
            "cancelled": False,
        }
        manifest = {
            "root": "source",
            "path": "Archive",
            "artifacts": [{"path": "Archive/Game.pak", "size": 4, "sha256": "a" * 64, "modifiedNs": 1}],
            "truncated": True,
        }
        with (
            mock.patch.object(self.host.toolchains.runner, "run", return_value=process),
            mock.patch.object(self.host.workspace, "manifest", return_value=manifest),
        ):
            accepted = self.call(
                "ue4ss_editor_package",
                {
                    "root": "source",
                    "projectPath": project.name,
                    "command": "BuildCookRun",
                    "arguments": [],
                    "artifactPath": "Archive",
                },
            )
            completed = self.wait_operation(accepted["operationId"])
        self.assertEqual(completed["status"], "succeeded")
        self.assertTrue(completed["currentState"]["artifactsTruncated"])
        self.assertTrue(completed["actualChanges"][0]["artifactsTruncated"])
        self.assertTrue(completed["evidence"][1]["artifactsTruncated"])

    def test_scenario_pass_failure_and_cancellation(self) -> None:
        passed = self.call(
            "ue4ss_scenario_run",
            {"scenario": {"name": "runtime", "steps": [{"id": "runtime", "action": "runtime_tool", "tool": "ue4ss_fake_runtime", "arguments": {"value": 9}, "capture": "result"}, {"id": "assert", "action": "assert", "actual": {"from": "result.arguments.value"}, "operator": "eq", "expected": 9}]}},
        )
        passed_result = self.wait_operation(passed["operationId"])
        self.assertEqual(passed_result["status"], "succeeded")
        failed = self.call("ue4ss_scenario_run", {"scenario": {"name": "failure", "steps": [{"id": "assert", "action": "assert", "actual": 1, "operator": "eq", "expected": 2}, {"id": "never", "action": "wait", "seconds": 0}]}})
        failed_result = self.wait_operation(failed["operationId"])
        self.assertEqual(failed_result["status"], "failed")
        steps = failed_result["currentState"]["failureContext"]["steps"]
        self.assertEqual([step["status"] for step in steps], ["failed", "not_executed"])
        waiting = self.call("ue4ss_scenario_run", {"scenario": {"name": "cancel", "steps": [{"action": "wait", "seconds": 5}]}})
        cancelled = self.call("ue4ss_task_cancel", {"operationId": waiting["operationId"]})
        self.assertTrue(cancelled["cancellationRequested"])
        cancelled_result = self.wait_operation(waiting["operationId"])
        self.assertEqual(cancelled_result["status"], "cancelled")
        events = self.call("ue4ss_events_read", {"cursor": 0, "limit": 1000})["currentState"]
        self.assertGreater(len(events["events"]), 0)


class TaskStateTests(unittest.TestCase):
    def test_cancelled_tool_error_preserves_accumulated_results(self) -> None:
        store = OperationStore("partial-test")
        registry = TaskRegistry(store, max_workers=1)
        partial_recorded = threading.Event()
        release_cancellation = threading.Event()

        def cancellable(context):
            context.set_partial_state({"root": "game", "phase": "prepared"})
            context.set_partial_state({"phase": "file-replaced"})
            context.record_change({"kind": "file.prepared", "path": "Mods/Test"})
            context.record_change({"kind": "file.replaced", "path": "Mods/Test"})
            context.record_evidence({"kind": "backup", "path": "backups/Test"})
            context.record_evidence({"kind": "sha256", "value": "abc"})
            context.output("replacement prepared")
            context.output("replacement committed")
            partial_recorded.set()
            context.cancelled.wait(2)
            release_cancellation.wait(2)
            context.check_cancelled("test.cancelled")
            return {}

        submitted = registry.submit("test_partial_cancel", "starting", cancellable)
        try:
            self.assertTrue(partial_recorded.wait(2))
            first_cancel = registry.cancel(submitted["operationId"])
            second_cancel = registry.cancel(submitted["operationId"])
            self.assertEqual(second_cancel["updatedAt"], first_cancel["updatedAt"])
            self.assertTrue(second_cancel["cancellationRequested"])
            release_cancellation.set()
            deadline = time.monotonic() + 2
            result = store.get(submitted["operationId"]).as_dict()
            while time.monotonic() < deadline:
                result = store.get(submitted["operationId"]).as_dict()
                if result["status"] == "cancelled":
                    break
                time.sleep(0.01)
            self.assertEqual(result["status"], "cancelled")
            self.assertEqual(result["currentState"]["root"], "game")
            self.assertEqual(result["currentState"]["phase"], "file-replaced")
            self.assertEqual(
                result["actualChanges"],
                [
                    {"kind": "file.prepared", "path": "Mods/Test"},
                    {"kind": "file.replaced", "path": "Mods/Test"},
                ],
            )
            self.assertEqual(
                result["evidence"],
                [
                    {"kind": "backup", "path": "backups/Test"},
                    {"kind": "sha256", "value": "abc"},
                ],
            )
            self.assertEqual(result["recentOutput"], ["replacement prepared", "replacement committed"])
            self.assertEqual(result["errors"][0]["code"], "operation.cancelled")
        finally:
            release_cancellation.set()
            registry.shutdown()

    def test_operation_history_is_strictly_bounded(self) -> None:
        store = OperationStore("history-test", max_history=3)
        operation_ids = [store.result(f"target-{index}")["operationId"] for index in range(5)]

        self.assertEqual(
            [operation["operationId"] for operation in store.list()],
            list(reversed(operation_ids[2:])),
        )
        for operation_id in operation_ids[:2]:
            with self.assertRaisesRegex(ToolError, "was not found"):
                store.get(operation_id)

    def test_operation_history_is_byte_bounded_and_events_are_summaries(self) -> None:
        events: list[tuple[str, dict[str, object]]] = []
        store = OperationStore("history-bytes", lambda kind, payload: events.append((kind, payload)), max_history_bytes=1200)
        first_id = store.result("first", state={"payload": "a" * 800})["operationId"]
        second_id = store.result("second", state={"payload": "b" * 800})["operationId"]

        with self.assertRaisesRegex(ToolError, "was not found"):
            store.get(first_id)
        self.assertEqual(store.get(second_id).current_state["payload"], "b" * 800)
        self.assertTrue(events)
        for _, payload in events:
            self.assertNotIn("currentState", payload)
            self.assertNotIn("actualChanges", payload)
            self.assertNotIn("evidence", payload)


if __name__ == "__main__":
    unittest.main()
