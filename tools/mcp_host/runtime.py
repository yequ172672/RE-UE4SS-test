from __future__ import annotations

import json
import threading
import urllib.error
import urllib.request
from typing import Any

from .model import ToolError


PROTOCOL_VERSION = "2025-06-18"


class RuntimeClient:
    def __init__(self, endpoint: str | None, timeout_seconds: float = 10.0) -> None:
        self.endpoint = endpoint.rstrip("/") if endpoint else None
        self.timeout_seconds = timeout_seconds
        self._request_id = 1
        self._lock = threading.Lock()
        self._initialized = False
        self._server_info: dict[str, Any] | None = None

    def health(self) -> dict[str, Any]:
        if not self.endpoint:
            return {"available": False, "reason": "runtime endpoint not configured"}
        url = self.endpoint[:-4] + "/health" if self.endpoint.endswith("/mcp") else self.endpoint + "/health"
        try:
            with urllib.request.urlopen(url, timeout=self.timeout_seconds) as response:
                data = json.loads(response.read())
                return {"available": response.status == 200, "endpoint": self.endpoint, **data}
        except (OSError, urllib.error.URLError, json.JSONDecodeError) as error:
            return {"available": False, "endpoint": self.endpoint, "reason": str(error)}

    def initialize(self) -> dict[str, Any]:
        result = self.rpc(
            "initialize",
            {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {},
                "clientInfo": {"name": "ue4ss-mcp-host", "version": "1.0.0"},
            },
            ensure_initialized=False,
        )
        self._initialized = True
        self._server_info = result
        return result

    def tools(self) -> list[dict[str, Any]]:
        self._ensure_initialized()
        result = self.rpc("tools/list", {})
        return list(result.get("tools", []))

    def resources(self) -> list[dict[str, Any]]:
        self._ensure_initialized()
        result = self.rpc("resources/list", {})
        return list(result.get("resources", []))

    def call_tool(self, name: str, arguments: dict[str, Any]) -> dict[str, Any]:
        self._ensure_initialized()
        return self.rpc("tools/call", {"name": name, "arguments": arguments})

    def read_resource(self, uri: str) -> dict[str, Any]:
        self._ensure_initialized()
        return self.rpc("resources/read", {"uri": uri})

    def rpc(self, method: str, params: dict[str, Any], *, ensure_initialized: bool = True) -> dict[str, Any]:
        if not self.endpoint:
            raise ToolError(
                "lifecycle.runtime_unavailable",
                "The embedded UE4SS runtime endpoint is not configured",
                stage="runtime.connect",
                suggested_actions=["Start the host with --runtime-endpoint http://127.0.0.1:<port>/mcp"],
            )
        if ensure_initialized:
            self._ensure_initialized()
        with self._lock:
            request_id = self._request_id
            self._request_id += 1
        payload = json.dumps({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params}).encode("utf-8")
        request = urllib.request.Request(
            self.endpoint,
            method="POST",
            data=payload,
            headers={
                "Content-Type": "application/json",
                "Accept": "application/json, text/event-stream",
                "MCP-Protocol-Version": PROTOCOL_VERSION,
            },
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout_seconds) as response:
                envelope = json.loads(response.read())
        except urllib.error.HTTPError as error:
            body = error.read().decode("utf-8", errors="replace")
            raise ToolError(
                "transport.runtime_http_error",
                f"Embedded runtime returned HTTP {error.code}",
                stage="runtime.request",
                context={"status": error.code, "body": body[:8192], "method": method},
                retryable=error.code >= 500,
            ) from error
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            raise ToolError(
                "lifecycle.runtime_disconnected",
                str(error),
                stage="runtime.request",
                context={"endpoint": self.endpoint, "method": method},
                retryable=True,
                suggested_actions=["Confirm the current game session MCP endpoint and reconnect"],
            ) from error
        except json.JSONDecodeError as error:
            raise ToolError("transport.runtime_invalid_json", str(error), stage="runtime.response", context={"method": method}) from error
        if "error" in envelope:
            error = envelope["error"]
            data = error.get("data") if isinstance(error, dict) else None
            code = data.get("code") if isinstance(data, dict) and isinstance(data.get("code"), str) else "runtime.rpc_error"
            message = error.get("message", "Embedded runtime RPC failed") if isinstance(error, dict) else str(error)
            raise ToolError(code, message, stage="runtime.rpc", context={"rpcError": error, "method": method})
        return envelope.get("result", {})

    def _ensure_initialized(self) -> None:
        if not self._initialized:
            self.initialize()
