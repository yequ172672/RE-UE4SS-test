#!/usr/bin/env python3

import argparse
import json
import sys
import urllib.error
import urllib.parse
import urllib.request

PROTOCOL_VERSION = "2025-06-18"
LARGE_REQUEST_ID = 9_007_199_254_740_993


def request(url, *, method="GET", payload=None, protocol=None, origin=None):
    headers = {"Accept": "application/json, text/event-stream"}
    data = None
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    if protocol is not None:
        headers["MCP-Protocol-Version"] = protocol
    if origin is not None:
        headers["Origin"] = origin

    outgoing = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(outgoing, timeout=10) as response:
            return response.status, dict(response.headers), response.read().decode("utf-8")
    except urllib.error.HTTPError as error:
        return error.code, dict(error.headers), error.read().decode("utf-8")


def rpc(endpoint, request_id, method, params, *, protocol=PROTOCOL_VERSION, origin=None):
    return request(
        endpoint,
        method="POST",
        payload={"jsonrpc": "2.0", "id": request_id, "method": method, "params": params},
        protocol=protocol,
        origin=origin,
    )


def parse_json(body):
    return json.loads(body) if body else None


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def call_tool(endpoint, request_id, name, arguments):
    status, _, body = rpc(endpoint, request_id, "tools/call", {"name": name, "arguments": arguments})
    payload = parse_json(body)
    require(status == 200, f"{name} transport failed: HTTP {status}: {body}")
    if "result" in payload:
        return payload["result"]["structuredContent"]
    if "error" in payload:
        return {"rpcError": payload["error"]}
    raise RuntimeError(f"{name} returned neither result nor error: {body}")


def run(endpoint, expect_runtime_ready):
    endpoint = endpoint.rstrip("/")
    health_url = urllib.parse.urljoin(f"{endpoint}/", "../health")
    checks = {}

    status, _, body = request(health_url)
    health = parse_json(body)
    require(status == 200 and health["status"] == "ok", f"health failed: HTTP {status}: {body}")
    checks["health"] = health

    status, _, body = rpc(
        endpoint,
        LARGE_REQUEST_ID,
        "initialize",
        {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {},
            "clientInfo": {"name": "ue4ss-mcp-smoke-test", "version": "1.0"},
        },
        protocol=None,
    )
    initialized = parse_json(body)
    require(status == 200, f"initialize failed: HTTP {status}: {body}")
    require(initialized["id"] == LARGE_REQUEST_ID, f"request id lost precision: {initialized['id']}")
    require(initialized["result"]["protocolVersion"] == PROTOCOL_VERSION, "unexpected negotiated protocol version")
    checks["initialize"] = {"id": initialized["id"], "serverInfo": initialized["result"]["serverInfo"]}

    status, _, body = rpc(endpoint, "tools", "tools/list", {})
    tools = parse_json(body)
    require(status == 200, f"tool discovery failed: HTTP {status}: {body}")
    tool_names = {tool["name"] for tool in tools["result"]["tools"]}
    required_tools = {
        "ue4ss_session_snapshot",
        "ue4ss_objects_query",
        "ue4ss_asset_load",
        "ue4ss_property_watch_start",
        "ue4ss_property_watches_list",
        "ue4ss_property_watch_read",
        "ue4ss_property_watch_stop",
        "ue4ss_lua_mod_control",
        "ue4ss_dumper_profile",
        "ue4ss_dumper_start",
    }
    require(required_tools <= tool_names, f"tool discovery is missing: {sorted(required_tools - tool_names)}")
    checks["tools"] = sorted(tool_names)

    status, _, body = rpc(
        endpoint,
        "logs",
        "tools/call",
        {"name": "ue4ss_logs_read", "arguments": {"cursor": 0, "maxBytes": 4096}},
    )
    logs = parse_json(body)
    log_result = logs["result"]["structuredContent"]
    require(status == 200 and log_result["status"] == "succeeded", f"log read failed: HTTP {status}: {body}")
    checks["logs"] = {"nextCursor": log_result["currentState"]["nextCursor"], "path": log_result["currentState"]["path"]}

    status, _, body = rpc(endpoint, "snapshot", "tools/call", {"name": "ue4ss_session_snapshot", "arguments": {}})
    snapshot = parse_json(body)["result"]["structuredContent"]
    if expect_runtime_ready:
        require(status == 200 and snapshot["status"] == "succeeded", f"runtime is not ready: HTTP {status}: {body}")
    elif snapshot["status"] != "succeeded":
        require(snapshot["errors"][0]["code"] == "lifecycle.runtime_not_ready", f"unexpected lifecycle failure: {body}")
    checks["runtime"] = snapshot["status"]
    if expect_runtime_ready:
        query = call_tool(endpoint, "query", "ue4ss_objects_query", {"className": "GameViewportClient", "includeDerived": True, "limit": 10})
        require(query["status"] == "succeeded" and query["currentState"]["objects"], f"object query failed: {query}")
        handle = query["currentState"]["objects"][0]["handle"]

        inspected = call_tool(endpoint, "inspect", "ue4ss_object_inspect", {"handle": handle, "limit": 1000})
        require(inspected["status"] == "succeeded" and inspected["currentState"]["properties"], f"object inspection failed: {inspected}")
        property_name = inspected["currentState"]["properties"][0]["name"]

        watch = call_tool(
            endpoint,
            "watch-start",
            "ue4ss_property_watch_start",
            {"handle": handle, "propertyName": property_name, "historyLimit": 10},
        )
        require(watch["status"] == "succeeded", f"property watch start failed: {watch}")
        watch_id = watch["currentState"]["watchId"]
        history = call_tool(endpoint, "watch-read", "ue4ss_property_watch_read", {"watchId": watch_id, "cursor": 0, "limit": 10})
        require(history["status"] == "succeeded" and history["currentState"]["events"], f"property watch read failed: {history}")
        stopped = call_tool(endpoint, "watch-stop", "ue4ss_property_watch_stop", {"watchId": watch_id})
        require(stopped["status"] == "succeeded" and stopped["currentState"]["stopped"], f"property watch stop failed: {stopped}")
        stopped_again = call_tool(endpoint, "watch-stop-again", "ue4ss_property_watch_stop", {"watchId": watch_id})
        require(stopped_again["status"] == "succeeded" and stopped_again["currentState"]["stopped"], f"idempotent watch stop failed: {stopped_again}")

        invalid_query = call_tool(endpoint, "invalid-query", "ue4ss_objects_query", {"name": "[", "regex": True})
        require(
            invalid_query["rpcError"]["code"] == -32602 and invalid_query["rpcError"]["data"]["code"] == "transport.invalid_arguments",
            f"invalid regex was not rejected: {invalid_query}",
        )
        missing_asset = call_tool(endpoint, "missing-asset", "ue4ss_asset_load", {"objectPath": "/Game/UE4SS/Missing.Missing"})
        require(
            missing_asset["status"] == "failed" and missing_asset["errors"][0]["code"] == "asset.not_found",
            f"missing asset returned the wrong failure: {missing_asset}",
        )
        missing_mod = call_tool(endpoint, "missing-mod", "ue4ss_lua_mod_control", {"name": "UE4SS_Missing_Mod", "action": "restart"})
        require(
            missing_mod["status"] == "failed" and missing_mod["errors"][0]["code"] == "mod.not_found",
            f"missing mod returned the wrong failure: {missing_mod}",
        )
        dumpers = call_tool(endpoint, "dumpers", "ue4ss_dumper_profile", {})
        require(
            dumpers["status"] == "succeeded" and len(dumpers["currentState"]["dumpers"]) == 7,
            f"dumper profile failed: {dumpers}",
        )
        checks["runtimeFeatures"] = {
            "queryReturned": query["currentState"]["returned"],
            "watchedProperty": property_name,
            "watchEvents": len(history["currentState"]["events"]),
            "dumperKinds": sorted(dumpers["currentState"]["dumpers"]),
            "failureCodes": [
                invalid_query["rpcError"]["data"]["code"],
                missing_asset["errors"][0]["code"],
                missing_mod["errors"][0]["code"],
            ],
        }

    status, _, body = rpc(endpoint, "origin", "tools/list", {}, origin="http://localhost.attacker.example")
    require(status == 403, f"non-loopback Origin was accepted: HTTP {status}: {body}")
    checks["originRejection"] = status

    status, _, body = rpc(endpoint, "version", "tools/list", {}, protocol="2024-11-05")
    require(status == 400, f"unsupported protocol version was accepted: HTTP {status}: {body}")
    checks["protocolRejection"] = status

    status, _, body = rpc(endpoint, "missing", "tools/call", {"name": "missing_tool", "arguments": {}})
    missing_tool = parse_json(body)
    require(status == 200 and missing_tool["error"]["code"] == -32602, f"unknown tool did not return -32602: HTTP {status}: {body}")
    checks["unknownTool"] = missing_tool["error"]["code"]

    status, headers, body = request(endpoint, method="GET")
    require(status == 405 and headers.get("Allow") == "POST", f"GET /mcp did not return 405 Allow: POST: HTTP {status}: {body}")
    checks["getMethod"] = status

    return checks


def main():
    parser = argparse.ArgumentParser(description="Exercise a running UE4SS MCP endpoint with success and failure scenarios.")
    parser.add_argument("--endpoint", default="http://127.0.0.1:8182/mcp")
    parser.add_argument("--expect-runtime-ready", action="store_true")
    args = parser.parse_args()

    try:
        checks = run(args.endpoint, args.expect_runtime_ready)
    except Exception as error:
        print(f"MCP smoke test failed: {error}", file=sys.stderr)
        return 1

    print(json.dumps(checks, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
