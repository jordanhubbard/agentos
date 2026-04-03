"""
agentOS HTTP shim server — bridges Python SDK calls to agentOS C IPC.

Run as a sidecar alongside agentOS::

    python -m agentos_sdk.shim [--socket /tmp/agentos.sock] [--port 8765]

The shim exposes a small FastAPI HTTP interface AND a Unix-socket
JSON-lines interface on the same socket path.  The Python SDK uses
the Unix socket directly; external tools (curl, test harnesses) can
use the HTTP API.

HTTP endpoints
──────────────
POST /msg                  send_message / publish
POST /store                store a key/value pair in MemFS
GET  /recall               semantic recall from MemFS
GET  /tools                list registered tools
POST /tool/{name}          invoke a named tool

Unix-socket protocol
────────────────────
The shim also listens on the Unix socket and speaks the same
newline-delimited JSON protocol used by AgentContext._Connection.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import signal
import struct
import sys
import uuid
from contextlib import asynccontextmanager
from typing import Any, AsyncIterator

log = logging.getLogger(__name__)

# ── configuration ──────────────────────────────────────────────────────────────
DEFAULT_SOCKET = "/tmp/agentos.sock"
DEFAULT_HTTP_PORT = 8765

# ── in-process mock store (fallback when agentOS is not available) ─────────────
_store: dict[str, str] = {}
_memory_log: list[str] = []
_tools: dict[str, dict[str, Any]] = {
    "web_search": {"description": "Search the web", "input_schema": '{"type":"object"}'},
    "code_exec":  {"description": "Execute code",   "input_schema": '{"type":"object"}'},
}


# ══════════════════════════════════════════════════════════════════════════════
# Low-level agentOS C IPC bridge
# ══════════════════════════════════════════════════════════════════════════════

class AgentOSBridge:
    """
    Thin wrapper around the agentOS C IPC.

    In production this would open the seL4 endpoint via a compiled C
    extension.  Here we provide a socket-based shim that proxies to
    the agentOS dev-shell port when available, or falls back to the
    mock in-process store.
    """

    def __init__(self, agentos_dev_port: int = 9999) -> None:
        self._port = agentos_dev_port
        self._available = False

    async def probe(self) -> bool:
        """Check whether the agentOS dev-shell is reachable."""
        try:
            _, writer = await asyncio.wait_for(
                asyncio.open_connection("127.0.0.1", self._port), timeout=0.5
            )
            writer.close()
            await writer.wait_closed()
            self._available = True
        except Exception:
            self._available = False
        return self._available

    # ── msgbus ─────────────────────────────────────────────────────────────────

    async def msgbus_send(self, params: dict[str, Any]) -> dict[str, Any]:
        if not self._available:
            return {"ack": f"[mock] delivered to {params.get('to', '?')}"}
        # Production: pack into seL4 message registers and call MSGBUS_OP_SEND_DIRECT
        return await self._c_call("MSGBUS_OP_SEND_DIRECT", params)

    async def msgbus_publish(self, params: dict[str, Any]) -> dict[str, Any]:
        if not self._available:
            return {"ok": True}
        return await self._c_call("MSGBUS_OP_PUBLISH", params)

    # ── memfs ──────────────────────────────────────────────────────────────────

    async def memfs_write(self, params: dict[str, Any]) -> dict[str, Any]:
        if not self._available:
            key = params.get("path", str(uuid.uuid4()))
            value = params.get("content", "")
            _store[key] = value
            _memory_log.append(value)
            return {"ok": True}
        return await self._c_call("MEMFS_OP_WRITE", params)

    async def memfs_recall(self, params: dict[str, Any]) -> dict[str, Any]:
        if not self._available:
            query = params.get("query", "").lower()
            k = int(params.get("k", 5))
            results = [v for v in _memory_log if query in v.lower()][-k:]
            return {"entries": results}
        return await self._c_call("MEMFS_OP_LIST", params)

    # ── toolsvc ────────────────────────────────────────────────────────────────

    async def toolsvc_list(self, params: dict[str, Any]) -> dict[str, Any]:
        if not self._available:
            return {"tools": list(_tools.keys())}
        return await self._c_call("TOOLSVC_OP_LIST", params)

    async def toolsvc_call(self, params: dict[str, Any]) -> dict[str, Any]:
        if not self._available:
            name = params.get("tool", "unknown")
            return {"tool": name, "result": f"[mock] {name} executed", "args": params.get("args", {})}
        return await self._c_call("TOOLSVC_OP_CALL", params)

    async def toolsvc_register(self, params: dict[str, Any]) -> dict[str, Any]:
        if not self._available:
            _tools[params["name"]] = params
            return {"ok": True}
        return await self._c_call("TOOLSVC_OP_REGISTER", params)

    # ── modelsvc ───────────────────────────────────────────────────────────────

    async def modelsvc_query(self, params: dict[str, Any]) -> dict[str, Any]:
        if not self._available:
            prompt = params.get("prompt", "")
            return {"response": f"[mock] Response to: {prompt[:80]}", "tokens_used": 42}
        return await self._c_call("MODELSVC_OP_QUERY", params)

    # ── capstore ───────────────────────────────────────────────────────────────

    async def capstore_list(self, params: dict[str, Any]) -> dict[str, Any]:
        if not self._available:
            return {"caps": [{"cap_id": 1, "type": "CAP_TYPE_TOOL", "rights": 7}]}
        return await self._c_call("CAPSTORE_OP_LIST", params)

    # ── internals ──────────────────────────────────────────────────────────────

    async def _c_call(self, opcode: str, params: dict[str, Any]) -> dict[str, Any]:
        """
        Placeholder for the real seL4 C IPC call.

        In a full implementation this would:
        1. Pack params into seL4 message registers (MR0-MR3).
        2. Call seL4_Call() on the appropriate endpoint.
        3. Unpack the response from message registers.

        For the shim we forward the request to the agentOS dev-shell
        on localhost:self._port.
        """
        try:
            reader, writer = await asyncio.open_connection("127.0.0.1", self._port)
            frame = json.dumps({"opcode": opcode, "params": params}) + "\n"
            writer.write(frame.encode())
            await writer.drain()
            response_line = await asyncio.wait_for(reader.readline(), timeout=5.0)
            writer.close()
            await writer.wait_closed()
            return json.loads(response_line)
        except Exception as exc:
            log.warning("C bridge call %s failed: %s", opcode, exc)
            return {"error": str(exc)}


# ══════════════════════════════════════════════════════════════════════════════
# Unix-socket JSON-lines server (used by AgentContext)
# ══════════════════════════════════════════════════════════════════════════════

OP_HANDLERS: dict[str, str] = {
    "agent.init":        "agent_init",
    "msgbus.send":       "msgbus_send",
    "msgbus.publish":    "msgbus_publish",
    "msgbus.subscribe":  "noop",
    "msgbus.unsubscribe": "noop",
    "memfs.write":       "memfs_write",
    "memfs.recall":      "memfs_recall",
    "toolsvc.list":      "toolsvc_list",
    "toolsvc.call":      "toolsvc_call",
    "toolsvc.register":  "toolsvc_register",
    "modelsvc.query":    "modelsvc_query",
    "capstore.list":     "capstore_list",
}


async def _handle_socket_client(
    reader: asyncio.StreamReader,
    writer: asyncio.StreamWriter,
    bridge: AgentOSBridge,
) -> None:
    peer = writer.get_extra_info("peername", "unknown")
    log.debug("New socket client: %s", peer)
    try:
        while True:
            line = await reader.readline()
            if not line:
                break
            try:
                req: dict[str, Any] = json.loads(line)
            except json.JSONDecodeError:
                continue

            req_id = req.get("id", "")
            op = req.get("op", "")
            params = req.get("params", {})

            result: Any = None
            error: str | None = None

            method_name = OP_HANDLERS.get(op)
            if method_name == "noop":
                result = {"ok": True}
            elif method_name and hasattr(bridge, method_name):
                try:
                    result = await getattr(bridge, method_name)(params)
                except Exception as exc:
                    error = str(exc)
            elif op == "agent.init":
                result = {"ok": True, "shim_version": "1.0.0"}
            else:
                error = f"Unknown operation: {op!r}"

            response = json.dumps({"id": req_id, "result": result, "error": error}) + "\n"
            writer.write(response.encode())
            await writer.drain()
    except asyncio.CancelledError:
        pass
    except Exception as exc:
        log.warning("Socket client error: %s", exc)
    finally:
        writer.close()


async def run_unix_server(socket_path: str, bridge: AgentOSBridge) -> None:
    """Start the Unix-socket JSON-lines server."""
    # Remove stale socket
    try:
        os.unlink(socket_path)
    except FileNotFoundError:
        pass

    server = await asyncio.start_unix_server(
        lambda r, w: _handle_socket_client(r, w, bridge),
        path=socket_path,
    )
    os.chmod(socket_path, 0o600)
    log.info("agentOS shim listening on %s", socket_path)
    async with server:
        await server.serve_forever()


# ══════════════════════════════════════════════════════════════════════════════
# FastAPI HTTP server
# ══════════════════════════════════════════════════════════════════════════════

def build_app(bridge: AgentOSBridge):  # type: ignore[return]
    """Build and return the FastAPI application."""
    try:
        from fastapi import FastAPI, HTTPException, Path
        from fastapi.responses import JSONResponse
        from pydantic import BaseModel
    except ImportError as exc:
        raise ImportError(
            "FastAPI is required for the HTTP shim: pip install agentos-sdk[shim]"
        ) from exc

    @asynccontextmanager
    async def lifespan(app: FastAPI) -> AsyncIterator[None]:
        await bridge.probe()
        log.info("agentOS bridge available: %s", bridge._available)
        yield

    app = FastAPI(title="agentOS Python Shim", version="1.0.0", lifespan=lifespan)

    # ── request models ─────────────────────────────────────────────────────────

    class MsgRequest(BaseModel):
        agent_id: str
        to: str
        content: str
        type: str = "AOS_MSG_TEXT"

    class StoreRequest(BaseModel):
        agent_id: str
        key: str
        value: str
        scope: str = "private"

    class ToolCallBody(BaseModel):
        agent_id: str
        args: dict[str, Any] = {}

    class ModelRequest(BaseModel):
        agent_id: str
        prompt: str
        model_id: str | None = None
        temperature: float = 0.7
        max_tokens: int = 1024
        system: str | None = None

    # ── endpoints ──────────────────────────────────────────────────────────────

    @app.post("/msg", summary="Send a direct message via MsgBus")
    async def post_msg(req: MsgRequest) -> JSONResponse:
        result = await bridge.msgbus_send({
            "from": req.agent_id,
            "to": req.to,
            "type": req.type,
            "content": req.content,
        })
        return JSONResponse(result)

    @app.post("/store", summary="Write a value to AgentFS (MemFS)")
    async def post_store(req: StoreRequest) -> JSONResponse:
        flags = 0x07
        if req.scope == "shared":
            flags |= 0x20
        result = await bridge.memfs_write({
            "agent_id": req.agent_id,
            "path": req.key,
            "content": req.value,
            "flags": flags,
        })
        return JSONResponse(result)

    @app.get("/recall", summary="Semantic recall from AgentFS")
    async def get_recall(agent_id: str, query: str, k: int = 5) -> JSONResponse:
        result = await bridge.memfs_recall({
            "agent_id": agent_id,
            "query": query,
            "k": k,
        })
        return JSONResponse(result)

    @app.get("/tools", summary="List registered tools from ToolSvc")
    async def get_tools(agent_id: str = "anonymous") -> JSONResponse:
        result = await bridge.toolsvc_list({"agent_id": agent_id})
        return JSONResponse(result)

    @app.post("/tool/{name}", summary="Invoke a named tool via ToolSvc")
    async def post_tool(
        name: str = Path(..., description="Tool name"),
        body: ToolCallBody = ...,
    ) -> JSONResponse:
        result = await bridge.toolsvc_call({
            "agent_id": body.agent_id,
            "tool": name,
            "args": body.args,
        })
        return JSONResponse(result)

    @app.post("/model", summary="Send an inference request to ModelSvc")
    async def post_model(req: ModelRequest) -> JSONResponse:
        result = await bridge.modelsvc_query({
            "agent_id": req.agent_id,
            "prompt": req.prompt,
            "model_id": req.model_id,
            "temperature": req.temperature,
            "max_tokens": req.max_tokens,
            "system": req.system,
        })
        return JSONResponse(result)

    @app.get("/health", summary="Health check")
    async def health() -> JSONResponse:
        return JSONResponse({"status": "ok", "agentos_available": bridge._available})

    return app


# ══════════════════════════════════════════════════════════════════════════════
# Entry point
# ══════════════════════════════════════════════════════════════════════════════

async def _run(socket_path: str, http_port: int) -> None:
    bridge = AgentOSBridge()
    await bridge.probe()

    try:
        import uvicorn  # type: ignore[import]
        from fastapi import FastAPI
    except ImportError:
        log.warning("FastAPI/uvicorn not installed — HTTP shim disabled (pip install agentos-sdk[shim])")
        await run_unix_server(socket_path, bridge)
        return

    app = build_app(bridge)

    config = uvicorn.Config(app, host="127.0.0.1", port=http_port, log_level="info")
    http_server = uvicorn.Server(config)

    await asyncio.gather(
        run_unix_server(socket_path, bridge),
        http_server.serve(),
    )


def main() -> None:
    import argparse

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s")

    parser = argparse.ArgumentParser(description="agentOS Python shim server")
    parser.add_argument("--socket", default=DEFAULT_SOCKET, help="Unix socket path")
    parser.add_argument("--port", type=int, default=DEFAULT_HTTP_PORT, help="HTTP port")
    args = parser.parse_args()

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)

    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, loop.stop)

    try:
        loop.run_until_complete(_run(args.socket, args.port))
    finally:
        loop.close()


if __name__ == "__main__":
    main()
