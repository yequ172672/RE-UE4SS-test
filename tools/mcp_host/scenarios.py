from __future__ import annotations

import json
import re
import time
from typing import Any

from .model import ToolError, utc_now
from .runtime import RuntimeClient
from .tasks import TaskContext
from .workspace import Workspace


class ScenarioRunner:
    def __init__(self, workspace: Workspace, runtime: RuntimeClient) -> None:
        self.workspace = workspace
        self.runtime = runtime

    def run(self, context: TaskContext, scenario: dict[str, Any]) -> dict[str, Any]:
        name = scenario.get("name")
        steps = scenario.get("steps")
        if not isinstance(name, str) or not name:
            raise ToolError("scenario.invalid", "Scenario requires a non-empty name", stage="scenario.validate")
        if not isinstance(steps, list) or not steps:
            raise ToolError("scenario.invalid", "Scenario requires at least one step", stage="scenario.validate")

        captures: dict[str, Any] = {}
        results: list[dict[str, Any]] = []
        started_at = utc_now()
        for index, step in enumerate(steps):
            context.check_cancelled("scenario.execute")
            if not isinstance(step, dict):
                raise ToolError("scenario.invalid_step", "Every scenario step must be an object", stage="scenario.validate", context={"index": index})
            step_id = str(step.get("id") or f"step-{index + 1}")
            action = step.get("action")
            context.progress(f"scenario {step_id}: {action}", index / len(steps))
            try:
                value = self._execute_step(context, action, step, captures)
                result = {"id": step_id, "action": action, "status": "passed", "value": value, "timestamp": utc_now()}
                capture = step.get("capture")
                if isinstance(capture, str) and capture:
                    captures[capture] = value
            except ToolError as error:
                terminal_status = error.status if error.status in {"cancelled", "indeterminate"} else "failed"
                cancelled = terminal_status == "cancelled"
                result = {"id": step_id, "action": action, "status": terminal_status, "error": error.as_dict(), "timestamp": utc_now()}
                results.append(result)
                for remaining_index, remaining in enumerate(steps[index + 1 :], index + 1):
                    results.append(
                        {
                            "id": str(remaining.get("id") or f"step-{remaining_index + 1}") if isinstance(remaining, dict) else f"step-{remaining_index + 1}",
                            "action": remaining.get("action") if isinstance(remaining, dict) else None,
                            "status": "not_executed",
                        }
                    )
                raise ToolError(
                    "operation.cancelled"
                    if cancelled
                    else "operation.indeterminate"
                    if terminal_status == "indeterminate"
                    else "scenario.assertion_failed"
                    if action == "assert"
                    else "scenario.step_failed",
                    f"Scenario '{name}' was {terminal_status} at step '{step_id}'",
                    stage="scenario.execute",
                    status=terminal_status,
                    context={"scenario": name, "steps": results, "captures": captures, "cause": error.as_dict()},
                    suggested_actions=[] if cancelled else ["Inspect the captured evidence before deciding whether the scenario is safe to replay"],
                ) from error
            results.append(result)

        report = {
            "name": name,
            "status": "passed",
            "startedAt": started_at,
            "completedAt": utc_now(),
            "steps": results,
            "captures": captures,
        }
        result_root = scenario.get("resultRoot")
        result_path = scenario.get("resultPath")
        changes: list[dict[str, Any]] = []
        evidence: list[dict[str, Any]] = [{"kind": "scenario", "source": name, "report": report}]
        if isinstance(result_root, str) and isinstance(result_path, str):
            written = self.workspace.write(result_root, result_path, json.dumps(report, ensure_ascii=False, indent=2) + "\n")
            changes.extend(written["actualChanges"])
            evidence.extend(written["evidence"])
        return {"currentState": report, "actualChanges": changes, "evidence": evidence}

    def _execute_step(self, context: TaskContext, action: Any, step: dict[str, Any], captures: dict[str, Any]) -> Any:
        if action == "runtime_tool":
            tool = step.get("tool")
            arguments = self._resolve_value(step.get("arguments", {}), captures)
            if not isinstance(tool, str) or not isinstance(arguments, dict):
                raise ToolError("scenario.invalid_step", "runtime_tool requires tool and object arguments", stage="scenario.runtime_tool")
            result = self.runtime.call_tool(tool, arguments)
            if result.get("isError"):
                structured = result.get("structuredContent", result)
                runtime_status = structured.get("status") if isinstance(structured, dict) else None
                raise ToolError(
                    "operation.indeterminate" if runtime_status == "indeterminate" else "scenario.runtime_tool_failed",
                    f"Runtime tool '{tool}' did not produce a verified result",
                    stage="scenario.runtime_tool",
                    status="indeterminate" if runtime_status == "indeterminate" else "failed",
                    context={"result": structured},
                )
            return result.get("structuredContent", result)
        if action == "file_read":
            return self.workspace.read(
                str(step["root"]),
                str(step["path"]),
                int(step.get("offset", 0)),
                int(step.get("maxBytes", 65536)),
                str(step.get("encoding", "utf-8")),
            )
        if action == "file_exists":
            scoped = self.workspace.resolve(str(step["root"]), str(step["path"]))
            return {"exists": scoped.path.exists(), "type": "directory" if scoped.path.is_dir() else "file" if scoped.path.is_file() else None}
        if action == "wait":
            seconds = min(max(float(step.get("seconds", 0)), 0), 300)
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                context.check_cancelled("scenario.wait")
                time.sleep(min(0.1, deadline - time.monotonic()))
            return {"waitedSeconds": seconds}
        if action == "assert":
            actual = self._resolve_value(step.get("actual"), captures)
            expected = self._resolve_value(step.get("expected"), captures)
            operator = str(step.get("operator", "eq"))
            passed = self._compare(operator, actual, expected)
            if not passed:
                raise ToolError(
                    "scenario.assertion_failed",
                    str(step.get("message") or f"Assertion {operator} failed"),
                    stage="scenario.assert",
                    context={"operator": operator, "actual": actual, "expected": expected},
                )
            return {"operator": operator, "actual": actual, "expected": expected}
        raise ToolError("scenario.unknown_action", f"Unknown scenario action '{action}'", stage="scenario.execute", context={"action": action})

    def _resolve_value(self, value: Any, captures: dict[str, Any]) -> Any:
        if isinstance(value, dict) and set(value) == {"from"}:
            reference = value["from"]
            if not isinstance(reference, str):
                raise ToolError("scenario.invalid_reference", "Capture reference must be a string", stage="scenario.resolve")
            parts = reference.split(".")
            if not parts or parts[0] not in captures:
                raise ToolError("scenario.capture_not_found", f"Capture '{parts[0] if parts else reference}' was not found", stage="scenario.resolve")
            current: Any = captures[parts[0]]
            for part in parts[1:]:
                if isinstance(current, dict) and part in current:
                    current = current[part]
                elif isinstance(current, list) and part.isdigit() and int(part) < len(current):
                    current = current[int(part)]
                else:
                    raise ToolError("scenario.reference_not_found", f"Reference '{reference}' does not exist", stage="scenario.resolve")
            return current
        if isinstance(value, dict):
            return {key: self._resolve_value(item, captures) for key, item in value.items()}
        if isinstance(value, list):
            return [self._resolve_value(item, captures) for item in value]
        return value

    @staticmethod
    def _compare(operator: str, actual: Any, expected: Any) -> bool:
        if operator == "eq":
            return actual == expected
        if operator == "ne":
            return actual != expected
        if operator == "contains":
            return expected in actual
        if operator == "not_contains":
            return expected not in actual
        if operator == "matches":
            return isinstance(actual, str) and isinstance(expected, str) and re.search(expected, actual) is not None
        if operator == "exists":
            return actual is not None
        if operator == "not_exists":
            return actual is None
        if operator == "gt":
            return actual > expected
        if operator == "gte":
            return actual >= expected
        if operator == "lt":
            return actual < expected
        if operator == "lte":
            return actual <= expected
        if operator == "truthy":
            return bool(actual)
        if operator == "falsy":
            return not actual
        raise ToolError("scenario.unknown_operator", f"Unknown assertion operator '{operator}'", stage="scenario.assert")
