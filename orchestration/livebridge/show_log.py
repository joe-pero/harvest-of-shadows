"""
show_log.py — lightweight structured logging for after-the-fact manual audit.

NOT automation, NOT self-modifying — a plain append-only JSONL file
(logs/show_events.jsonl) Joe reads by hand (or with jq/pandas) whenever he
wants, weekly or after the season. No rotation, no summarization, no
alerting built in.

Hard requirement this exists to satisfy: logging must add ZERO latency to
the visitor-facing path, and can never delay or crash a real cue. The
mechanism:

  caller -> log() -> queue.put_nowait() -> returns immediately (in-memory,
            O(1), never touches disk, never raises)
  worker thread (separate, daemon) -> queue.get() (blocks itself, not the
            caller) -> best-effort disk write, wrapped in try/except so a
            disk error, a full disk, or a bad field can never propagate
            back into the caller.

Bounded queue: if the worker ever falls behind (stalled disk), new events
are dropped rather than blocking the caller or growing memory without
bound. A dropped-event counter is itself logged best-effort, so a real
problem is visible in the log after the fact rather than silently invisible.

Accepted tradeoff, stated plainly: a hard process kill can lose a few
buffered-but-unwritten rows. Never blocking or crashing the show wins over
never losing a log line.
"""
from __future__ import annotations

import json
import queue
import threading
import time
from pathlib import Path


class ShowLog:
    def __init__(self, path: Path, maxsize: int = 1000) -> None:
        self._path = Path(path)
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._q: queue.Queue = queue.Queue(maxsize=maxsize)
        self._dropped = 0
        self._thread = threading.Thread(target=self._worker, daemon=True, name="show-log")
        self._thread.start()

    def log(self, event: str, **fields) -> None:
        """Fire-and-forget. Never blocks, never raises, regardless of what
        happens to the underlying file."""
        try:
            row = {"ts": time.time(), "iso": time.strftime("%Y-%m-%dT%H:%M:%S"), "event": event, **fields}
            self._q.put_nowait(row)
        except queue.Full:
            self._dropped += 1
        except Exception:
            pass   # never let a bad field or serialization issue reach the caller

    def _worker(self) -> None:
        while True:
            row = self._q.get()   # blocks THIS thread only
            try:
                with open(self._path, "a") as f:
                    f.write(json.dumps(row, default=str) + "\n")
            except Exception:
                pass   # best-effort — a disk problem here never surfaces to any caller
            if self._dropped:
                try:
                    with open(self._path, "a") as f:
                        f.write(json.dumps({
                            "ts": time.time(), "iso": time.strftime("%Y-%m-%dT%H:%M:%S"),
                            "event": "log_queue_full", "dropped": self._dropped,
                        }) + "\n")
                except Exception:
                    pass
                self._dropped = 0
