"""Khonsu Python SDK: make tool side effects replayable.

    import khonsu

    @khonsu.tool
    def run_tests(path: str) -> dict:
        ...  # touches the filesystem, network, clock, a database...

Under ``khonsu record`` the tool runs normally and its arguments and result are
recorded. Under ``khonsu replay`` the recorded result is returned **without
running the tool**, so side effects are not repeated and the run is
reproducible. Outside Khonsu the decorator does nothing.

Tools whose effect *is* the workspace (writing a file the agent reads back)
should use ``@khonsu.tool(replay="rerun")``: on replay they run again for their
effect, a result that differs from the recording is reported, and the agent gets
the recorded result, so the replay stays on the recorded path. Khonsu's
workspace snapshots verify the files they produced.

Arguments and results must be JSON-compatible (tuples come back as lists).
"""

from __future__ import annotations

import builtins
import functools
import sys
import inspect
import json
import os
import urllib.error
import urllib.request
from typing import Any, Callable, TypeVar, overload

__all__ = ["tool", "active", "mode", "ToolError", "KhonsuDivergence"]
__version__ = "0.3.0"

F = TypeVar("F", bound=Callable[..., Any])


class ToolError(Exception):
    """A tool raised an exception that is not a builtin type; replayed by name and message."""

    def __init__(self, type_name: str, message: str):
        super().__init__(f"{type_name}: {message}")
        self.type_name = type_name
        self.message = message


class KhonsuDivergence(RuntimeError):
    """Replay met a tool call that is not in the recording (arguments differ)."""


def _url() -> str | None:
    return os.environ.get("KHONSU_TOOL_URL") or None


def active() -> bool:
    """True when running under ``khonsu record`` / ``replay`` / ``fork``."""
    return _url() is not None


def mode() -> str | None:
    """``"record"``, ``"replay"`` or None."""
    return os.environ.get("KHONSU_MODE") if active() else None


def _post(path: str, payload: dict) -> dict:
    data = json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()
    req = urllib.request.Request(_url() + path, data=data, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=600) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        body = e.read().decode(errors="replace")
        if e.code == 409:
            try:
                msg = json.loads(body)["error"]["message"]
            except Exception:
                msg = body
            raise KhonsuDivergence(msg) from None
        raise RuntimeError(f"khonsu proxy error {e.code}: {body}") from None


def _jsonable(value: Any, what: str) -> Any:
    try:
        # Round-trip now so record and replay see exactly the same value.
        return json.loads(json.dumps(value, sort_keys=True, ensure_ascii=False))
    except (TypeError, ValueError) as e:
        raise TypeError(f"khonsu.tool: {what} must be JSON-serializable ({e})") from None


def _raise_recorded(error: dict) -> None:
    name, message = error.get("type", "Exception"), error.get("message", "")
    exc_type = getattr(builtins, name, None)
    if isinstance(exc_type, type) and issubclass(exc_type, Exception):
        raise exc_type(message)
    raise ToolError(name, message)


@overload
def tool(fn: F) -> F: ...
@overload
def tool(*, name: str | None = None, replay: str = "skip") -> Callable[[F], F]: ...


def tool(fn: F | None = None, *, name: str | None = None, replay: str = "skip"):
    """Record a tool's calls. On replay, ``replay="skip"`` (default) returns the
    recorded result without running the tool; ``replay="rerun"`` runs it again for
    its effect, warns if its result differs, and returns the recorded result."""
    if replay not in ("skip", "rerun"):
        raise ValueError('khonsu.tool: replay must be "skip" or "rerun"')

    def decorate(f: F) -> F:
        tool_name = name or f"{f.__module__}.{f.__qualname__}"
        sig = inspect.signature(f)

        @functools.wraps(f)
        def wrapper(*args: Any, **kwargs: Any) -> Any:
            if not active():
                return f(*args, **kwargs)
            bound = sig.bind(*args, **kwargs)
            bound.apply_defaults()
            call = {"name": tool_name, "args": _jsonable(dict(bound.arguments), "arguments")}
            answer = _post("/begin", call)
            if answer.get("action") == "replay":
                outcome = answer["outcome"]
                if replay == "rerun":
                    # Run again for the effect; hand the agent the recorded result so the
                    # replay stays on the recorded path (pytest timings, PIDs, ... differ).
                    try:
                        fresh = {"ok": True, "result": _jsonable(f(*args, **kwargs), "the return value")}
                    except Exception as e:
                        fresh = {"ok": False, "error": {"type": type(e).__name__, "message": str(e)}}
                    if fresh != {k: outcome.get(k) for k in fresh}:
                        print(f"[khonsu] tool {tool_name} re-ran with a different result than recorded; "
                              "returning the recorded one", file=sys.stderr)
                if outcome.get("ok"):
                    return outcome.get("result")
                _raise_recorded(outcome.get("error", {}))
            token = answer["token"]
            try:
                result = _jsonable(f(*args, **kwargs), "the return value")
            except Exception as e:  # includes a result that is not JSON: the call is closed either way
                _post("/end", {"token": token,
                               "outcome": {"ok": False, "error": {"type": type(e).__name__, "message": str(e)}}})
                raise
            _post("/end", {"token": token, "outcome": {"ok": True, "result": result}})
            return result

        wrapper.__khonsu_tool__ = tool_name  # type: ignore[attr-defined]
        return wrapper  # type: ignore[return-value]

    return decorate(fn) if fn is not None else decorate
