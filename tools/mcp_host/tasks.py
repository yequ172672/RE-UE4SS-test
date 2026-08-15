from __future__ import annotations

from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass, field
import os
import signal
import subprocess
import threading
from typing import Any, Callable

from .model import OperationStore, ToolError


@dataclass(slots=True)
class TaskContext:
    operation_id: str
    store: OperationStore
    cancelled: threading.Event = field(default_factory=threading.Event)
    _process: subprocess.Popen[str] | None = None
    _lock: threading.Lock = field(default_factory=threading.Lock)

    def check_cancelled(self, stage: str) -> None:
        if self.cancelled.is_set():
            raise ToolError("operation.cancelled", "Operation was cancelled", stage=stage, status="cancelled")

    def progress(self, action: str, value: float | None = None) -> None:
        with self._lock:
            self.store.update(self.operation_id, current_action=action, progress=value)

    def set_partial_state(self, state: dict[str, Any], *, merge: bool = True) -> None:
        with self._lock:
            if merge:
                self.store.accumulate(self.operation_id, state=dict(state))
            else:
                self.store.update(self.operation_id, current_state=dict(state))

    def record_change(self, change: dict[str, Any]) -> None:
        with self._lock:
            self.store.accumulate(self.operation_id, changes=[dict(change)])

    def record_evidence(self, evidence: dict[str, Any]) -> None:
        with self._lock:
            self.store.accumulate(self.operation_id, evidence=[dict(evidence)])

    def output(self, line: str) -> None:
        with self._lock:
            self.store.append_output(self.operation_id, line)

    def attach_process(self, process: subprocess.Popen[str]) -> None:
        with self._lock:
            self._process = process
            if self.cancelled.is_set() and process.poll() is None:
                self.terminate_process(process)

    def detach_process(self) -> None:
        with self._lock:
            self._process = None

    def cancel(self) -> None:
        self.cancelled.set()
        with self._lock:
            if self._process and self._process.poll() is None:
                self.terminate_process(self._process)

    @staticmethod
    def terminate_process(process: subprocess.Popen[str]) -> None:
        if process.poll() is not None:
            return
        if os.name == "nt":
            subprocess.run(
                ["taskkill.exe", "/PID", str(process.pid), "/T", "/F"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
        else:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass


TaskCallable = Callable[[TaskContext], dict[str, Any]]


class TaskRegistry:
    def __init__(self, store: OperationStore, max_workers: int = 4) -> None:
        self.store = store
        self._executor = ThreadPoolExecutor(max_workers=max(1, max_workers), thread_name_prefix="ue4ss-mcp-task")
        self._contexts: dict[str, TaskContext] = {}
        self._futures: dict[str, Future[None]] = {}
        self._lock = threading.RLock()

    def submit(self, target: str, action: str, callable_: TaskCallable) -> dict[str, Any]:
        operation = self.store.create(target, status="accepted", current_action=action)
        context = TaskContext(operation.operation_id, self.store)
        with self._lock:
            self._contexts[operation.operation_id] = context
            self._futures[operation.operation_id] = self._executor.submit(self._run, context, callable_)
        return operation.as_dict()

    def cancel(self, operation_id: str) -> dict[str, Any]:
        with self._lock:
            context = self._contexts.get(operation_id)
            operation = self.store.get(operation_id)
            if context is None:
                if operation.status in {"succeeded", "failed", "partial", "indeterminate", "cancelled", "rejected"}:
                    return operation.as_dict()
                raise ToolError("operation.not_found", f"Operation '{operation_id}' is not active", stage="operation.cancel")
            if operation.cancellation_requested:
                return operation.as_dict()
            operation = self.store.update(operation_id, cancellation_requested=True, current_action="cancellation requested")
            context.cancel()
            return operation.as_dict()

    def shutdown(self) -> None:
        with self._lock:
            contexts = list(self._contexts.values())
        for context in contexts:
            context.cancel()
        self._executor.shutdown(wait=True, cancel_futures=True)

    def _run(self, context: TaskContext, callable_: TaskCallable) -> None:
        self.store.update(context.operation_id, status="running")
        try:
            context.check_cancelled("operation.start")
            outcome = callable_(context)
            if context.cancelled.is_set():
                cancellation = ToolError("operation.cancelled", "Operation was cancelled after producing partial results", stage="operation.finish", status="cancelled")
                self.store.accumulate(
                    context.operation_id,
                    state=outcome.get("currentState"),
                    changes=outcome.get("actualChanges"),
                    evidence=outcome.get("evidence"),
                )
                operation = self.store.get(context.operation_id)
                self.store.update(
                    context.operation_id,
                    restart_required=operation.restart_required or bool(outcome.get("restartRequired")),
                    reload_required=operation.reload_required or bool(outcome.get("reloadRequired")),
                )
                self.store.fail(context.operation_id, cancellation)
            else:
                self.store.succeed(
                    context.operation_id,
                    state=outcome.get("currentState"),
                    changes=outcome.get("actualChanges"),
                    evidence=outcome.get("evidence"),
                    restart_required=bool(outcome.get("restartRequired")),
                    reload_required=bool(outcome.get("reloadRequired")),
                )
        except ToolError as error:
            self.store.fail(context.operation_id, error)
        except Exception as error:
            self.store.fail(
                context.operation_id,
                ToolError(
                    "operation.internal_error",
                    str(error),
                    stage="operation.execute",
                    context={"exceptionType": type(error).__name__},
                ),
            )
        finally:
            with self._lock:
                self._contexts.pop(context.operation_id, None)
                self._futures.pop(context.operation_id, None)
