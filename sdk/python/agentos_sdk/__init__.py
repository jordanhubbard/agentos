"""
agentos_sdk — Python SDK for agentOS (seL4 microkernel agent OS).

Provides async Python access to agentOS services (MsgBus, MemFS, ToolSvc,
ModelSvc, CapStore) via a Unix-socket shim that bridges to the C SDK.

Quick start::

    import asyncio
    from agentos_sdk import AgentContext

    async def main():
        async with AgentContext("my_agent") as ctx:
            tools = await ctx.list_tools()
            result = await ctx.call_tool("web_search", query="agentOS seL4")
            await ctx.store("notes/search", result["result"])
            memories = await ctx.recall("agentOS")
            print(memories)

    asyncio.run(main())

Using the @tool decorator::

    from agentos_sdk import tool

    @tool(description="Add two integers")
    async def add(x: int, y: int) -> int:
        return x + y

    print(add.schema)

Using the EventBus::

    from agentos_sdk import EventBus

    async def handler(event):
        print("Received:", event)

    async def main():
        async with EventBus() as bus:
            await bus.subscribe("system.events", handler)
            await bus.publish("system.events", {"type": "ping"})
            await asyncio.sleep(0.1)

    asyncio.run(main())
"""

from __future__ import annotations

from agentos_sdk.context import AgentContext, AgentOSError
from agentos_sdk.eventbus import EventBus
from agentos_sdk.tools import (
    ToolDescriptor,
    dispatch,
    get_registered_tools,
    get_tool,
    tool,
)

# Back-compat alias used by older examples
EventBusClient = EventBus

__all__ = [
    # Core context
    "AgentContext",
    "AgentOSError",
    # Event bus
    "EventBus",
    "EventBusClient",
    # Tool system
    "tool",
    "ToolDescriptor",
    "get_tool",
    "get_registered_tools",
    "dispatch",
]

__version__ = "0.1.0"
__author__ = "agentOS contributors"
