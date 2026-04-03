# agentos-sdk — Python SDK for agentOS

Python async interface to [agentOS](../../DESIGN.md) — a seL4 microkernel-based
agent operating system.  The SDK communicates with agentOS services (MsgBus,
MemFS, ToolSvc, ModelSvc, CapStore) via a Unix-socket shim that bridges to the
C SDK without requiring seL4 headers at the Python level.

Works in **mock mode** when agentOS is not running — ideal for development and
unit testing.

---

## Installation

```bash
# Core SDK (pure stdlib, no extra deps)
pip install agentos-sdk

# With HTTP shim sidecar (FastAPI + uvicorn)
pip install "agentos-sdk[shim]"

# With LangChain integration
pip install "agentos-sdk[langchain]"

# Everything
pip install "agentos-sdk[all]"

# Editable install from source
pip install -e "sdk/python[dev]"
```

---

## Quick Start

### Minimal example

```python
import asyncio
from agentos_sdk import AgentContext

async def main():
    # Connects to /tmp/agentos.sock; falls back to mock mode if not present
    async with AgentContext("my_agent") as ctx:
        # List tools registered in agentOS ToolSvc
        tools = await ctx.list_tools()
        print("Tools:", tools)

        # Store a note in AgentFS (MemFS)
        await ctx.store("notes/greeting", "Hello, agentOS!")

        # Semantic recall from AgentFS
        memories = await ctx.recall("greeting", k=3)
        print("Memories:", memories)

        # Invoke a tool via ToolSvc
        result = await ctx.call_tool("web_search", query="seL4 microkernel")
        print("Tool result:", result)

        # Direct message via MsgBus
        ack = await ctx.send_message("system.broadcast", "Agent online")
        print("Ack:", ack)

asyncio.run(main())
```

### Mock mode (no agentOS required)

```python
async with AgentContext("dev_agent", mock=True) as ctx:
    tools = await ctx.list_tools()   # returns ["web_search", "code_exec", ...]
    memories = await ctx.recall("test")  # returns []
```

---

## API Reference

### `AgentContext`

```python
AgentContext(
    agent_id: str,
    socket_path: str = "/tmp/agentos.sock",
    *,
    mock: bool = False,
)
```

| Method | Description |
|--------|-------------|
| `await connect()` | Open the socket connection (called by `async with`) |
| `await close()` | Close the connection |
| `await send_message(to, content) -> str` | Send direct message via MsgBus |
| `await publish(channel, payload)` | Publish event to named channel |
| `await store(key, value, scope="private")` | Write to AgentFS (MemFS) |
| `await recall(query, k=5) -> list[str]` | Semantic recall from AgentFS |
| `await list_tools() -> list[str]` | List tools from ToolSvc |
| `await call_tool(name, **kwargs) -> dict` | Invoke a tool via ToolSvc |
| `await query_model(prompt, **kwargs) -> str` | Inference via ModelSvc |
| `await list_caps() -> list[dict]` | List agent capabilities from CapStore |

### `@tool` decorator

```python
from agentos_sdk import tool

@tool(description="Compute the square of a number")
async def square(n: int) -> int:
    return n * n

# Access the descriptor
print(square.name)         # "square"
print(square.description)  # "Compute the square of a number"
print(square.schema)       # JSON Schema dict
```

- Registers the function in the in-process tool registry.
- JSON Schema is auto-generated from Python type hints.
- Works with both `async def` and `def` functions.

### `EventBus`

```python
from agentos_sdk import EventBus

async def on_event(event: dict) -> None:
    print("Event:", event)

async with EventBus() as bus:
    await bus.subscribe("system.events", on_event)
    await bus.publish("system.events", {"type": "heartbeat"})

    # One-shot wait
    event = await bus.wait_for("system.events", timeout=5.0)
```

| Method | Description |
|--------|-------------|
| `await connect()` | Connect to the shim socket |
| `await subscribe(channel, callback)` | Register a callback on a channel |
| `await unsubscribe(channel, callback=None)` | Remove subscription |
| `await publish(channel, event)` | Publish an event |
| `await wait_for(channel, predicate=None, timeout=None)` | One-shot event wait |

---

## HTTP Shim Sidecar

The shim bridges Python SDK calls to agentOS C IPC.  Run it alongside
agentOS:

```bash
agentos-shim --socket /tmp/agentos.sock --port 8765
```

### HTTP endpoints

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/msg` | Send a message via MsgBus |
| `POST` | `/store` | Write to AgentFS |
| `GET`  | `/recall?agent_id=X&query=Y&k=5` | Semantic recall |
| `GET`  | `/tools?agent_id=X` | List registered tools |
| `POST` | `/tool/{name}` | Invoke a tool |
| `POST` | `/model` | Model inference |
| `GET`  | `/health` | Health check |

Example:

```bash
curl -s http://127.0.0.1:8765/health | jq
curl -s "http://127.0.0.1:8765/tools?agent_id=demo" | jq
curl -s -X POST http://127.0.0.1:8765/tool/web_search \
     -H "Content-Type: application/json" \
     -d '{"agent_id":"demo","args":{"query":"seL4"}}' | jq
```

---

## LangChain Integration

```python
# examples/langchain_agent.py
python examples/langchain_agent.py --question "What is agentOS?" --mock
```

The example wraps `@tool`-decorated functions as LangChain `StructuredTool`
objects and drives an agent loop.  When `langchain` is not installed it falls
back to direct tool dispatch.

---

## Architecture

```
Python agent code
      │
      │  asyncio / Unix socket (JSON-lines)
      ▼
agentos_sdk.shim  ←── HTTP (FastAPI, port 8765)
      │
      │  seL4 C IPC (MSGBUS_OP_*, MEMFS_OP_*, TOOLSVC_OP_*, …)
      ▼
agentOS services
  ├── MsgBus   (channel pub/sub, direct messaging)
  ├── MemFS    (AgentFS — content-addressable memory)
  ├── ToolSvc  (tool registry & dispatch)
  ├── ModelSvc (LLM inference)
  └── CapStore (capability management)
```

The Python SDK never links against seL4 libraries.  The shim is the only
component that performs real C IPC.  When the shim is not reachable the SDK
automatically enters mock mode.

---

## Service Opcodes

The SDK maps Python methods to agentOS service opcodes:

| SDK method | Service | Opcode |
|------------|---------|--------|
| `send_message` | MsgBus | `MSGBUS_OP_SEND_DIRECT` (0x105) |
| `publish` | MsgBus | `MSGBUS_OP_PUBLISH` (0x104) |
| `store` | MemFS | `MEMFS_OP_WRITE` (0x400) |
| `recall` | MemFS | `MEMFS_OP_LIST` (0x403) |
| `list_tools` | ToolSvc | `TOOLSVC_OP_LIST` (0x203) |
| `call_tool` | ToolSvc | `TOOLSVC_OP_CALL` (0x202) |
| `query_model` | ModelSvc | `MODELSVC_OP_QUERY` (0x300) |
| `list_caps` | CapStore | `CAPSTORE_OP_LIST` (0x503) |

---

## Development

```bash
# Install in editable mode with dev extras
pip install -e "sdk/python[dev]"

# Run tests
pytest sdk/python/tests/

# Type check
mypy sdk/python/agentos_sdk/

# Lint
ruff check sdk/python/
```
