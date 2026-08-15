from __future__ import annotations

from dataclasses import dataclass, field
from datetime import UTC, datetime
import json
import threading
import uuid
from typing import Any, Callable


STATUSES = {"accepted", "running", "succeeded", "failed", "partial", "indeterminate", "cancelled", "rejected"}


def utc_now() -> str:
    return datetime.now(UTC).isoformat()


class ToolError(RuntimeError):
    def __init__(
        self,
        code: str,
        message: str,
        *,
        stage: str,
        context: dict[str, Any] | None = None,
        retryable: bool = False,
        suggested_actions: list[str] | None = None,
        status: str = "failed",
    ) -> None:
        super().__init__(message)
        self.code = code
        self.stage = stage
        self.context = context or {}
        self.retryable = retryable
        self.suggested_actions = suggested_actions or []
        self.status = status

    def as_dict(self) -> dict[str, Any]:
        return {
            "code": self.code,
            "stage": self.stage,
            "message": str(self),
            "context": self.context,
            "retryable": self.retryable,
        }


@dataclass(slots=True)
class Evidence:
    kind: str
    source: str
    data: dict[str, Any] = field(default_factory=dict)

    def as_dict(self) -> dict[str, Any]:
        return {"kind": self.kind, "source": self.source, **self.data}


@dataclass(slots=True)
class Operation:
    operation_id: str
    session_id: str
    target: str
    status: str
    created_at: str
    updated_at: str
    sequence: int
    current_action: str = ""
    progress: float | None = None
    actual_changes: list[dict[str, Any]] = field(default_factory=list)
    current_state: dict[str, Any] = field(default_factory=dict)
    evidence: list[dict[str, Any]] = field(default_factory=list)
    errors: list[dict[str, Any]] = field(default_factory=list)
    suggested_actions: list[str] = field(default_factory=list)
    restart_required: bool = False
    reload_required: bool = False
    recent_output: list[str] = field(default_factory=list)
    cancellation_requested: bool = False

    def summary_dict(self) -> dict[str, Any]:
        return {
            "operationId": self.operation_id,
            "sessionId": self.session_id,
            "target": self.target,
            "status": self.status,
            "createdAt": self.created_at,
            "updatedAt": self.updated_at,
            "currentAction": self.current_action,
            "progress": self.progress,
            "restartRequired": self.restart_required,
            "reloadRequired": self.reload_required,
            "recentOutput": self.recent_output[-20:],
            "cancellationRequested": self.cancellation_requested,
        }

    def as_dict(self) -> dict[str, Any]:
        return {
            "operationId": self.operation_id,
            "sessionId": self.session_id,
            "target": self.target,
            "status": self.status,
            "createdAt": self.created_at,
            "updatedAt": self.updated_at,
            "currentAction": self.current_action,
            "progress": self.progress,
            "actualChanges": self.actual_changes,
            "currentState": self.current_state,
            "evidence": self.evidence,
            "errors": self.errors,
            "suggestedActions": self.suggested_actions,
            "restartRequired": self.restart_required,
            "reloadRequired": self.reload_required,
            "recentOutput": self.recent_output[-100:],
            "cancellationRequested": self.cancellation_requested,
        }


class OperationStore:
    def __init__(
        self,
        session_id: str,
        on_change: Callable[[str, dict[str, Any]], None] | None = None,
        *,
        max_history: int = 4096,
        max_recent_output: int = 100,
        max_history_bytes: int = 64 * 1024 * 1024,
    ) -> None:
        if max_history < 1:
            raise ValueError("max_history must be positive")
        if max_recent_output < 1:
            raise ValueError("max_recent_output must be positive")
        if max_history_bytes < 1:
            raise ValueError("max_history_bytes must be positive")
        self.session_id = session_id
        self._on_change = on_change
        self._max_history = max_history
        self._max_recent_output = max_recent_output
        self._max_history_bytes = max_history_bytes
        self._history_bytes = 0
        self._operation_sizes: dict[str, int] = {}
        self._next_sequence = 1
        self._operations: dict[str, Operation] = {}
        self._lock = threading.RLock()

    def create(self, target: str, status: str = "accepted", current_action: str = "") -> Operation:
        if status not in STATUSES:
            raise ValueError(f"Invalid operation status: {status}")
        now = utc_now()
        operation = Operation(
            operation_id=f"{self.session_id}-{uuid.uuid4().hex[:12]}",
            session_id=self.session_id,
            target=target,
            status=status,
            created_at=now,
            updated_at=now,
            current_action=current_action,
            sequence=0,
        )
        with self._lock:
            operation.sequence = self._next_sequence
            self._next_sequence += 1
            self._operations[operation.operation_id] = operation
            self._record_size_locked(operation)
        self._publish(operation)
        return operation

    def get(self, operation_id: str) -> Operation:
        with self._lock:
            operation = self._operations.get(operation_id)
            if operation is None:
                raise ToolError(
                    "operation.not_found",
                    f"Operation '{operation_id}' was not found",
                    stage="operation.lookup",
                    context={"operationId": operation_id},
                )
            return operation

    def _record_size_locked(self, operation: Operation) -> None:
        serialized_size = len(json.dumps(operation.as_dict(), ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
        previous_size = self._operation_sizes.get(operation.operation_id, 0)
        self._operation_sizes[operation.operation_id] = serialized_size
        self._history_bytes += serialized_size - previous_size
        while len(self._operations) > 1 and (
            len(self._operations) > self._max_history or self._history_bytes > self._max_history_bytes
        ):
            candidates = [operation_id for operation_id in self._operations if operation_id != operation.operation_id]
            if not candidates:
                break
            oldest_id = min(candidates, key=lambda item: self._operations[item].sequence)
            self._operations.pop(oldest_id)
            self._history_bytes -= self._operation_sizes.pop(oldest_id, 0)

    def list(self, statuses: set[str] | None = None) -> list[dict[str, Any]]:
        return self.page(statuses=statuses, cursor=0, limit=self._max_history)["operations"]

    def page(self, *, statuses: set[str] | None = None, cursor: int = 0, limit: int = 100) -> dict[str, Any]:
        if cursor < 0:
            raise ValueError("cursor must not be negative")
        if limit < 1:
            raise ValueError("limit must be positive")
        with self._lock:
            matching = [
                operation
                for operation in self._operations.values()
                if (not statuses or operation.status in statuses) and (cursor == 0 or operation.sequence < cursor)
            ]
            matching.sort(key=lambda item: item.sequence, reverse=True)
            page = matching[:limit]
            next_cursor = page[-1].sequence if page else cursor
            return {
                "operations": [operation.summary_dict() for operation in page],
                "nextCursor": next_cursor,
                "endOfList": len(page) >= len(matching),
            }

    def update(self, operation_id: str, **changes: Any) -> Operation:
        with self._lock:
            operation = self.get(operation_id)
            for key, value in changes.items():
                if key == "status" and value not in STATUSES:
                    raise ValueError(f"Invalid operation status: {value}")
                if not hasattr(operation, key):
                    raise AttributeError(key)
                setattr(operation, key, value)
            operation.updated_at = utc_now()
            self._record_size_locked(operation)
            snapshot = operation.summary_dict()
        self._emit("operation.updated", snapshot)
        return operation

    def accumulate(
        self,
        operation_id: str,
        *,
        state: dict[str, Any] | None = None,
        changes: list[dict[str, Any]] | None = None,
        evidence: list[dict[str, Any]] | None = None,
    ) -> Operation:
        with self._lock:
            operation = self.get(operation_id)
            if state:
                operation.current_state.update(state)
            if changes:
                operation.actual_changes.extend(changes)
            if evidence:
                operation.evidence.extend(evidence)
            operation.updated_at = utc_now()
            self._record_size_locked(operation)
            snapshot = operation.summary_dict()
        self._emit("operation.updated", snapshot)
        return operation

    def append_output(self, operation_id: str, line: str) -> None:
        with self._lock:
            operation = self.get(operation_id)
            operation.recent_output.append(line.rstrip("\r\n"))
            if len(operation.recent_output) > self._max_recent_output:
                del operation.recent_output[: len(operation.recent_output) - self._max_recent_output]
            operation.updated_at = utc_now()
            self._record_size_locked(operation)
            snapshot = operation.summary_dict()
        self._emit("operation.output", snapshot)

    def succeed(
        self,
        operation_id: str,
        *,
        state: dict[str, Any] | None = None,
        changes: list[dict[str, Any]] | None = None,
        evidence: list[dict[str, Any]] | None = None,
        restart_required: bool = False,
        reload_required: bool = False,
    ) -> Operation:
        with self._lock:
            operation = self.get(operation_id)
            merged_state = dict(operation.current_state)
            merged_state.update(state or {})
            merged_changes = [*operation.actual_changes, *(changes or [])]
            merged_evidence = [*operation.evidence, *(evidence or [])]
            return self.update(
                operation_id,
                status="succeeded",
                progress=1.0,
                current_action="completed",
                current_state=merged_state,
                actual_changes=merged_changes,
                evidence=merged_evidence,
                restart_required=operation.restart_required or restart_required,
                reload_required=operation.reload_required or reload_required,
            )

    def fail(self, operation_id: str, error: ToolError) -> Operation:
        with self._lock:
            operation = self.get(operation_id)
            state = dict(operation.current_state)
            state["failureContext"] = error.context
            status = error.status
            if status == "failed" and (operation.actual_changes or operation.evidence or operation.current_state):
                status = "partial"
            suggested_actions = list(operation.suggested_actions)
            suggested_actions.extend(action for action in error.suggested_actions if action not in suggested_actions)
            return self.update(
                operation_id,
                status=status,
                current_action=error.stage,
                errors=[*operation.errors, error.as_dict()],
                current_state=state,
                suggested_actions=suggested_actions,
            )

    def result(
        self,
        target: str,
        *,
        state: dict[str, Any] | None = None,
        changes: list[dict[str, Any]] | None = None,
        evidence: list[dict[str, Any]] | None = None,
        restart_required: bool = False,
        reload_required: bool = False,
    ) -> dict[str, Any]:
        operation = self.create(target, status="succeeded", current_action="completed")
        return self.succeed(
            operation.operation_id,
            state=state,
            changes=changes,
            evidence=evidence,
            restart_required=restart_required,
            reload_required=reload_required,
        ).as_dict()

    def error_result(self, target: str, error: ToolError) -> dict[str, Any]:
        operation = self.create(target, status=error.status, current_action=error.stage)
        return self.fail(operation.operation_id, error).as_dict()

    def _publish(self, operation: Operation) -> None:
        self._emit("operation.created", operation.summary_dict())

    def _emit(self, kind: str, payload: dict[str, Any]) -> None:
        if self._on_change:
            self._on_change(kind, payload)


def content_result(operation: dict[str, Any], is_error: bool | None = None) -> dict[str, Any]:
    if is_error is None:
        is_error = operation.get("status") not in {"accepted", "running", "succeeded", "partial"}
    return {
        "content": [{"type": "text", "text": json.dumps(operation, ensure_ascii=False, separators=(",", ":"))}],
        "structuredContent": operation,
        "isError": is_error,
    }
