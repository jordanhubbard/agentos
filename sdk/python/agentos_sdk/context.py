"""
AgentContext — async Python interface to agentOS services.

Communicates with agentOS via a Unix socket (or HTTP shim) using a
newline-delimited JSON protocol that mirrors the C SDK's service opcodes.

Wire format (request):
    {"id": "<uuid>", "op": "<opcode>", "params": {...}}

Wire format (response):
    {"id": "<uuid>", "result": <any>, "error": null | "<msg>"}
"""

from __future__ import annotations

import asyncio
import json
import logging
import uuid
from typing import Any

log = logging.getLogger(__name__)

# ── mock-mode responses ────────────────────────────────────────────────────────
_MOCK_TOOLS: list[str] = ["web_search", "code_exec", "file_read"]
_mock_store: dict[str, str] = {}
_mock_memory: list[str] = []


class AgentOSError(Exception):
    """Raised when agentOS returns an error response."""


class _Connection:
    """
    Thin async wrapper around a Unix-socket or TCP connection to the shim.
    Handles framing (newline-delimited JSON) and in-flight request correlation.
    """

    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self._reader = reader
        self._writer = writer
        self._pending: dict[str, asyncio.Future[Any]] = {}
        self._recv_task: asyncio.Task[None] | None = None

    # ── public ─────────────────────────────────────────────────────────────────

    def start(self) -> None:
        self._recv_task = asyncio.get_event_loop().create_task(self._recv_loop())

    async def call(self, op: str, params: dict[str, Any]) -> Any:
        req_id = str(uuid.uuid4())
        future: asyncio.Future[Any] = asyncio.get_event_loop().create_future()
        self._pending[req_id] = future

        frame = json.dumps({"id": req_id, "op": op, "params": params}) + "\n"
        self._writer.write(frame.encode())
        await self._writer.drain()

        return await future

    async def close(self) -> None:
        if self._recv_task:
            self._recv_task.cancel()
            try:
                await self._recv_task
            except asyncio.CancelledError:
                pass
        self._writer.close()
        try:
            await self._writer.wait_closed()
        except Exception:
            pass

    # ── internals ──────────────────────────────────────────────────────────────

    async def _recv_loop(self) -> None:
        try:
            while True:
                line = await self._reader.readline()
                if not line:
                    break
                msg: dict[str, Any] = json.loads(line)
                req_id: str = msg.get("id", "")
                fut = self._pending.pop(req_id, None)
                if fut is None or fut.done():
                    continue
                if msg.get("error"):
                    fut.set_exception(AgentOSError(msg["error"]))
                else:
                    fut.set_result(msg.get("result"))
        except asyncio.CancelledError:
            pass
        except Exception as exc:
            log.warning("agentOS connection lost: %s", exc)
            for fut in self._pending.values():
                if not fut.done():
                    fut.set_exception(exc)


class AgentContext:
    """
    High-level async interface to agentOS services for Python/LLM agents.

    Parameters
    ----------
    agent_id:
        Logical identity string for this agent (e.g. ``"my_agent"``).
    socket_path:
        Path to the agentOS shim Unix socket.  When the socket does not
        exist the context falls back to *mock mode* automatically.
    mock:
        Force mock mode regardless of socket availability.
    """

    def __init__(
        self,
        agent_id: str,
        socket_path: str = "/tmp/agentos.sock",
        *,
        mock: bool = False,
    ) -> None:
        self.agent_id = agent_id
        self.socket_path = socket_path
        self._mock = mock
        self._conn: _Connection | None = None

    # ── lifecycle ──────────────────────────────────────────────────────────────

    async def connect(self) -> "AgentContext":
        """Open the socket connection.  Safe to call multiple times."""
        if self._mock or self._conn is not None:
            return self
        import os

        if not os.path.exists(self.socket_path):
            log.info("agentOS socket not found at %s — using mock mode", self.socket_path)
            self._mock = True
            return self

        try:
            reader, writer = await asyncio.open_unix_connection(self.socket_path)
            self._conn = _Connection(reader, writer)
            self._conn.start()
            # Announce ourselves to the shim
            await self._conn.call("agent.init", {"agent_id": self.agent_id})
            log.info("Connected to agentOS shim at %s", self.socket_path)
        except OSError as exc:
            log.warning("Cannot connect to agentOS shim: %s — using mock mode", exc)
            self._mock = True
        return self

    async def close(self) -> None:
        """Close the socket connection."""
        if self._conn:
            await self._conn.close()
            self._conn = None

    async def __aenter__(self) -> "AgentContext":
        return await self.connect()

    async def __aexit__(self, *_: Any) -> None:
        await self.close()

    # ── messaging (MsgBus) ─────────────────────────────────────────────────────

    async def send_message(self, to: str, content: str) -> str:
        """
        Send a direct message to another agent via the agentOS EventBus.

        Maps to ``MSGBUS_OP_SEND_DIRECT`` / ``AOS_MSG_TEXT``.

        Parameters
        ----------
        to:
            Destination agent ID or well-known name (e.g. ``"system.broadcast"``).
        content:
            UTF-8 message body.

        Returns
        -------
        str
            Acknowledgement or reply payload from the remote agent.
        """
        if self._mock:
            log.debug("[mock] send_message to=%s content=%r", to, content[:80])
            return f"[mock-ack] message delivered to {to!r}"

        result = await self._rpc("msgbus.send", {
            "from": self.agent_id,
            "to": to,
            "type": "AOS_MSG_TEXT",
            "content": content,
        })
        return str(result.get("ack", ""))

    async def publish(self, channel: str, payload: dict[str, Any]) -> None:
        """
        Publish an event to a named channel (``MSGBUS_OP_PUBLISH``).
        """
        if self._mock:
            log.debug("[mock] publish channel=%s payload=%r", channel, payload)
            return
        await self._rpc("msgbus.publish", {"channel": channel, "payload": payload})

    # ── memory (MemFS / AgentFS) ───────────────────────────────────────────────

    async def store(self, key: str, value: str, scope: str = "private") -> None:
        """
        Write a key/value pair into AgentFS (``MEMFS_OP_WRITE``).

        Parameters
        ----------
        key:
            Path-like storage key, e.g. ``"notes/summary"``.
        value:
            UTF-8 string content.
        scope:
            ``"private"`` (default) or ``"shared"`` — maps to MemFS flags.
        """
        if self._mock:
            _mock_store[f"{scope}:{key}"] = value
            _mock_memory.append(value)
            log.debug("[mock] store key=%s scope=%s", key, scope)
            return

        flags = 0x07  # RDWR | CREATE | TRUNC
        if scope == "shared":
            flags |= 0x20  # notional "shared" flag

        await self._rpc("memfs.write", {
            "agent_id": self.agent_id,
            "path": key,
            "content": value,
            "flags": flags,
        })

    async def recall(self, query: str, k: int = 5) -> list[str]:
        """
        Semantic memory recall from AgentFS (``MEMFS_OP_LIST`` + ranking).

        Parameters
        ----------
        query:
            Free-text query string.
        k:
            Maximum number of results to return.

        Returns
        -------
        list[str]
            Up to *k* stored strings most relevant to the query.
        """
        if self._mock:
            results = [v for v in _mock_memory if query.lower() in v.lower()][:k]
            if not results:
                results = list(_mock_memory[-k:])
            log.debug("[mock] recall query=%r -> %d results", query, len(results))
            return results

        result = await self._rpc("memfs.recall", {
            "agent_id": self.agent_id,
            "query": query,
            "k": k,
        })
        return list(result.get("entries", []))

    # ── tools (ToolSvc) ────────────────────────────────────────────────────────

    async def list_tools(self) -> list[str]:
        """
        Return the names of all tools registered in the agentOS ToolSvc
        (``TOOLSVC_OP_LIST``).
        """
        if self._mock:
            return list(_MOCK_TOOLS)

        result = await self._rpc("toolsvc.list", {"agent_id": self.agent_id})
        return list(result.get("tools", []))

    async def call_tool(self, name: str, **kwargs: Any) -> dict[str, Any]:
        """
        Invoke a registered agentOS tool (``TOOLSVC_OP_CALL``).

        Parameters
        ----------
        name:
            Tool name as returned by :meth:`list_tools`.
        **kwargs:
            Keyword arguments forwarded as the tool's JSON input payload.

        Returns
        -------
        dict
            Parsed JSON output from the tool.
        """
        if self._mock:
            log.debug("[mock] call_tool name=%s kwargs=%r", name, kwargs)
            return {"tool": name, "result": f"[mock] {name} executed", "args": kwargs}

        result = await self._rpc("toolsvc.call", {
            "agent_id": self.agent_id,
            "tool": name,
            "args": kwargs,
        })
        return dict(result)

    # ── model / inference (ModelSvc) ───────────────────────────────────────────

    async def query_model(
        self,
        prompt: str,
        *,
        model_id: str | None = None,
        temperature: float = 0.7,
        max_tokens: int = 1024,
        system: str | None = None,
        timeout_ms: int = 30_000,
    ) -> str:
        """
        Send an inference request to the agentOS ModelSvc
        (``MODELSVC_OP_QUERY``).

        Returns the generated text response.
        """
        if self._mock:
            log.debug("[mock] query_model prompt=%r", prompt[:60])
            return f"[mock response to: {prompt[:60]}...]"

        result = await self._rpc("modelsvc.query", {
            "agent_id": self.agent_id,
            "prompt": prompt,
            "model_id": model_id,
            "temperature": temperature,
            "max_tokens": max_tokens,
            "system": system,
            "timeout_ms": timeout_ms,
        })
        return str(result.get("response", ""))

    # ── capabilities ───────────────────────────────────────────────────────────

    async def list_caps(self) -> list[dict[str, Any]]:
        """Return capabilities held by this agent (``CAPSTORE_OP_LIST``)."""
        if self._mock:
            return [{"cap_id": 1, "type": "CAP_TYPE_TOOL", "rights": 7}]

        result = await self._rpc("capstore.list", {"agent_id": self.agent_id})
        return list(result.get("caps", []))

    # ── internals ──────────────────────────────────────────────────────────────

    async def _rpc(self, op: str, params: dict[str, Any]) -> dict[str, Any]:
        """Send an RPC to the shim and return the result dict."""
        if self._conn is None:
            raise AgentOSError("Not connected — call await ctx.connect() first")
        raw = await self._conn.call(op, params)
        if isinstance(raw, dict):
            return raw
        return {"value": raw}
