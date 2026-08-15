from __future__ import annotations

from collections import deque
from dataclasses import dataclass
import threading
import time
from typing import Any

from .model import utc_now


@dataclass(frozen=True, slots=True)
class Event:
    sequence: int
    kind: str
    timestamp: str
    data: dict[str, Any]

    def as_dict(self) -> dict[str, Any]:
        return {"sequence": self.sequence, "kind": self.kind, "timestamp": self.timestamp, "data": self.data}


class EventLog:
    def __init__(self, capacity: int = 4096) -> None:
        self._events: deque[Event] = deque(maxlen=max(128, capacity))
        self._next_sequence = 1
        self._condition = threading.Condition()

    def publish(self, kind: str, data: dict[str, Any]) -> Event:
        with self._condition:
            event = Event(self._next_sequence, kind, utc_now(), data)
            self._next_sequence += 1
            self._events.append(event)
            self._condition.notify_all()
            return event

    def read(self, cursor: int = 0, limit: int = 100, wait_seconds: float = 0) -> dict[str, Any]:
        limit = min(max(limit, 1), 1000)
        deadline = time.monotonic() + max(wait_seconds, 0)
        with self._condition:
            while not any(event.sequence > cursor for event in self._events) and wait_seconds > 0:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                self._condition.wait(remaining)

            available = [event for event in self._events if event.sequence > cursor]
            page = available[:limit]
            oldest = self._events[0].sequence if self._events else self._next_sequence
            next_cursor = page[-1].sequence if page else max(cursor, oldest - 1)
            return {
                "events": [event.as_dict() for event in page],
                "nextCursor": next_cursor,
                "truncated": len(available) > len(page),
                "cursorExpired": cursor != 0 and cursor < oldest - 1,
                "oldestCursor": oldest - 1,
            }
