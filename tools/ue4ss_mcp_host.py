from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import signal
import sys
from typing import Any
import threading

from mcp_host import McpHostServer


def load_config(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as file:
        config = json.load(file)
    if not isinstance(config, dict):
        raise ValueError("Host configuration must be a JSON object")
    roots = config.get("roots")
    if not isinstance(roots, dict) or not roots:
        raise ValueError("Host configuration requires a non-empty 'roots' object")
    base = path.parent
    config["roots"] = {
        name: str((base / value).resolve()) if not Path(value).is_absolute() else str(Path(value).resolve())
        for name, value in roots.items()
        if isinstance(name, str) and isinstance(value, str)
    }
    if not config["roots"]:
        raise ValueError("Host configuration did not contain a valid root")
    state_directory = config.get("stateDirectory", ".ue4ss-mcp")
    if not isinstance(state_directory, str):
        raise ValueError("'stateDirectory' must be a string")
    config["stateDirectory"] = str((base / state_directory).resolve()) if not Path(state_directory).is_absolute() else str(Path(state_directory).resolve())
    tools = config.get("tools", {})
    if not isinstance(tools, dict) or not all(isinstance(key, str) and isinstance(value, str) for key, value in tools.items()):
        raise ValueError("'tools' must be an object of executable names to paths")
    config["tools"] = {
        name: str((base / value).resolve()) if ("/" in value or "\\" in value) and not Path(value).is_absolute() else value
        for name, value in tools.items()
    }
    return config


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="UE4SS AI MCP host-side workspace, build and validation server")
    parser.add_argument("--config", required=True, type=Path, help="JSON configuration with explicit roots and optional runtime endpoint")
    parser.add_argument("--runtime-endpoint", help="Override embedded runtime endpoint, for example http://127.0.0.1:8182/mcp")
    parser.add_argument("--port", type=int, help="Override the loopback listen port; 0 selects an ephemeral port")
    parser.add_argument("--print-endpoint", action="store_true", help="Print the selected endpoint as one JSON line after binding")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        config = load_config(arguments.config.resolve())
        bind_address = config.get("bindAddress", "127.0.0.1")
        port = arguments.port if arguments.port is not None else config.get("port", 0)
        runtime_endpoint = arguments.runtime_endpoint or os.environ.get("UE4SS_MCP_RUNTIME_ENDPOINT") or config.get("runtimeEndpoint")
        if not isinstance(bind_address, str) or isinstance(port, bool) or not isinstance(port, int) or not 0 <= port <= 65535:
            raise ValueError("Invalid bindAddress or port")
        server = McpHostServer(
            roots=config["roots"],
            state_directory=config["stateDirectory"],
            runtime_endpoint=runtime_endpoint,
            tools=config["tools"],
            bind_address=bind_address,
            port=port,
            max_workers=int(config.get("maxWorkers", 4)),
        )
        server.start()
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(json.dumps({"status": "failed", "error": str(error)}, ensure_ascii=False), file=sys.stderr)
        return 2

    if arguments.print_endpoint:
        print(json.dumps({"status": "ready", "endpoint": server.endpoint, "sessionId": server.session_id}, ensure_ascii=False), flush=True)

    stopping = threading.Event()

    def stop(_signal: int, _frame: object) -> None:
        stopping.set()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    try:
        stopping.wait()
    finally:
        server.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
