"""
@tool decorator — register Python functions as agentOS tools.

Usage::

    from agentos_sdk.tools import tool

    @tool(description="Add two numbers")
    async def add(x: int, y: int) -> int:
        return x + y

    # add.descriptor  →  ToolDescriptor(name="add", description=..., schema=...)
    # add.handler     →  the original async callable

The decorator inspects Python type hints to produce a JSON Schema
compatible with agentOS ``aos_tool_def_t.input_schema``.
"""

from __future__ import annotations

import asyncio
import functools
import inspect
import json
import logging
from dataclasses import dataclass, field
from typing import Any, Callable, get_type_hints

log = logging.getLogger(__name__)

# ── JSON Schema type mapping ───────────────────────────────────────────────────
_PY_TO_JSON: dict[Any, str] = {
    int: "integer",
    float: "number",
    str: "string",
    bool: "boolean",
    bytes: "string",
    list: "array",
    dict: "object",
    type(None): "null",
}

# Registry of all decorated tools in the process
_REGISTRY: dict[str, "ToolDescriptor"] = {}


# ── ToolDescriptor ─────────────────────────────────────────────────────────────

@dataclass
class ToolDescriptor:
    """
    Describes an agentOS tool, mirroring ``aos_tool_def_t``.

    Attributes
    ----------
    name:
        Unique tool name (snake_case recommended).
    description:
        Human/agent-readable description shown in ToolSvc listings.
    schema:
        JSON Schema dict describing the tool's input parameters.
    handler:
        Async callable that implements the tool.
    output_schema:
        Optional JSON Schema dict describing the tool's output.
    """

    name: str
    description: str
    schema: dict[str, Any]
    handler: Callable[..., Any]
    output_schema: dict[str, Any] = field(default_factory=lambda: {"type": "object"})

    def to_agentos_def(self) -> dict[str, Any]:
        """Serialise to the agentOS ``toolsvc.register`` wire format."""
        return {
            "name": self.name,
            "description": self.description,
            "input_schema": json.dumps(self.schema),
            "output_schema": json.dumps(self.output_schema),
        }

    async def __call__(self, **kwargs: Any) -> Any:
        """Invoke the handler, accepting keyword arguments."""
        if asyncio.iscoroutinefunction(self.handler):
            return await self.handler(**kwargs)
        return self.handler(**kwargs)


# ── @tool decorator ────────────────────────────────────────────────────────────

def tool(
    func: Callable[..., Any] | None = None,
    *,
    name: str | None = None,
    description: str | None = None,
) -> Any:
    """
    Decorator that registers a function as an agentOS tool.

    Can be used with or without arguments::

        @tool
        async def my_tool(x: int) -> str: ...

        @tool(name="custom_name", description="Does something")
        async def my_tool(x: int) -> str: ...

    Parameters
    ----------
    func:
        The function to decorate (when used without parentheses).
    name:
        Override the tool name (defaults to the function name).
    description:
        Override the description (defaults to the docstring first line).

    Returns
    -------
    ToolDescriptor
        A callable descriptor with ``.name``, ``.description``, ``.schema``,
        and ``.handler`` attributes.
    """

    def decorator(fn: Callable[..., Any]) -> ToolDescriptor:
        tool_name = name or fn.__name__
        tool_desc = description or _extract_description(fn)
        schema = _build_schema(fn, tool_name)

        descriptor = ToolDescriptor(
            name=tool_name,
            description=tool_desc,
            schema=schema,
            handler=fn,
        )

        # Preserve function metadata
        functools.update_wrapper(descriptor, fn)  # type: ignore[arg-type]

        # Register globally
        if tool_name in _REGISTRY:
            log.warning("Tool %r re-registered — overwriting previous definition", tool_name)
        _REGISTRY[tool_name] = descriptor
        log.debug("Registered tool %r", tool_name)

        return descriptor

    if func is not None:
        # @tool used without parentheses
        return decorator(func)
    return decorator


# ── registry helpers ───────────────────────────────────────────────────────────

def get_registered_tools() -> dict[str, ToolDescriptor]:
    """Return a copy of the in-process tool registry."""
    return dict(_REGISTRY)


def get_tool(name: str) -> ToolDescriptor | None:
    """Look up a tool by name."""
    return _REGISTRY.get(name)


async def dispatch(name: str, **kwargs: Any) -> Any:
    """
    Invoke a registered tool by name.

    Raises
    ------
    KeyError
        If no tool with *name* is registered.
    """
    descriptor = _REGISTRY.get(name)
    if descriptor is None:
        raise KeyError(f"No tool registered with name {name!r}")
    return await descriptor(**kwargs)


# ── schema generation ──────────────────────────────────────────────────────────

def _build_schema(fn: Callable[..., Any], tool_name: str) -> dict[str, Any]:
    """Build a JSON Schema dict from the function's type hints."""
    try:
        hints = get_type_hints(fn)
    except Exception:
        hints = {}

    sig = inspect.signature(fn)
    properties: dict[str, Any] = {}
    required: list[str] = []

    for param_name, param in sig.parameters.items():
        if param_name in ("self", "cls"):
            continue

        py_type = hints.get(param_name, Any)
        json_type = _resolve_json_type(py_type)

        prop: dict[str, Any] = {"type": json_type}

        # Pull per-parameter docstring if available (Google/NumPy style TODO)
        prop_desc = _param_description(fn, param_name)
        if prop_desc:
            prop["description"] = prop_desc

        properties[param_name] = prop

        if param.default is inspect.Parameter.empty:
            required.append(param_name)

    schema: dict[str, Any] = {
        "type": "object",
        "title": tool_name,
        "properties": properties,
    }
    if required:
        schema["required"] = required

    return schema


def _resolve_json_type(py_type: Any) -> str:
    """Map a Python type annotation to a JSON Schema type string."""
    import types as _types

    # Handle Optional[X] → X | None
    origin = getattr(py_type, "__origin__", None)
    if origin is _types.UnionType or str(origin) in ("typing.Union",):
        args = [a for a in py_type.__args__ if a is not type(None)]
        if args:
            return _resolve_json_type(args[0])
        return "null"

    # Handle list[X]
    if origin in (list,):
        return "array"

    # Handle dict[X, Y]
    if origin in (dict,):
        return "object"

    return _PY_TO_JSON.get(py_type, "string")


def _extract_description(fn: Callable[..., Any]) -> str:
    """Return the first non-empty line of the function's docstring."""
    doc = inspect.getdoc(fn) or ""
    for line in doc.splitlines():
        line = line.strip()
        if line:
            return line
    return fn.__name__.replace("_", " ")


def _param_description(fn: Callable[..., Any], param_name: str) -> str:
    """
    Attempt to extract a per-parameter description from a Google-style
    docstring (``Args:\n    param_name: description``).
    """
    doc = inspect.getdoc(fn) or ""
    in_args = False
    for line in doc.splitlines():
        stripped = line.strip()
        if stripped.lower() in ("args:", "arguments:", "parameters:"):
            in_args = True
            continue
        if in_args:
            if stripped and not stripped.startswith(" ") and stripped.endswith(":"):
                # New section — stop
                break
            if stripped.startswith(f"{param_name}:") or stripped.startswith(f"{param_name} ("):
                return stripped.split(":", 1)[-1].strip()
    return ""
