"""
simple_agent.py — minimal agentOS Python SDK example (~30 lines).

Run with agentOS running:   python simple_agent.py
Run without agentOS (mock): python simple_agent.py --mock
"""

import asyncio
import argparse
from agentos_sdk import AgentContext, tool


@tool(description="Add two integers and return their sum")
async def add(x: int, y: int) -> int:
    return x + y


async def main(mock: bool) -> None:
    async with AgentContext("simple_agent", mock=mock) as ctx:
        # List available tools
        tools = await ctx.list_tools()
        print("Available tools:", tools)

        # Store and recall a memory
        await ctx.store("notes/greeting", "Hello from simple_agent!")
        memories = await ctx.recall("greeting", k=3)
        print("Recalled:", memories)

        # Invoke the local @tool directly via AgentContext.call_tool
        result = await ctx.call_tool("add", x=3, y=4)
        print("add(3, 4) =", result)

        # Send a message (no-op in mock mode)
        ack = await ctx.send_message("system.broadcast", "Agent online")
        print("Message ack:", ack)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--mock", action="store_true", default=True,
                        help="Run in mock mode (default: True)")
    args = parser.parse_args()
    asyncio.run(main(args.mock))
