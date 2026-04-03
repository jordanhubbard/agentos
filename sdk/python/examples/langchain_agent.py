"""
langchain_agent.py — LangChain agent wired to agentOS tools via @tool decorator.

Prerequisites::

    pip install agentos-sdk[langchain]
    # Set ANTHROPIC_API_KEY or OPENAI_API_KEY in your environment

Run::

    python langchain_agent.py --mock        # mock mode (no agentOS required)
    python langchain_agent.py               # live mode (agentOS socket required)
"""

from __future__ import annotations

import argparse
import asyncio
import os

from agentos_sdk import AgentContext, tool


# ── Define tools with @tool decorator ─────────────────────────────────────────

@tool(description="Search the web for current information on a topic")
async def web_search(query: str, max_results: int = 5) -> dict:
    """
    Args:
        query: Search query string.
        max_results: Maximum number of results to return.
    """
    # In real usage AgentContext.call_tool("web_search", ...) would forward
    # this to agentOS ToolSvc.  Here we return a stub for illustration.
    return {
        "query": query,
        "results": [
            {"title": f"Result {i} for '{query}'", "url": f"https://example.com/{i}"}
            for i in range(1, max_results + 1)
        ],
    }


@tool(description="Store a note in agentOS memory")
async def remember(key: str, value: str) -> str:
    """
    Args:
        key: Storage key (path-like, e.g. 'notes/meeting').
        value: Content to store.
    """
    # Normally you'd call ctx.store(key, value) — shown below in main()
    return f"Stored {key!r}"


@tool(description="Recall notes from agentOS memory")
async def recall_notes(query: str) -> list:
    """
    Args:
        query: Free-text query to match against stored notes.
    """
    return [f"[mock] memory entry matching '{query}'"]


# ── LangChain integration ──────────────────────────────────────────────────────

def _build_langchain_tools():
    """Wrap @tool descriptors as LangChain StructuredTool objects."""
    from langchain_core.tools import StructuredTool  # type: ignore[import]
    from agentos_sdk import get_registered_tools

    lc_tools = []
    for descriptor in get_registered_tools().values():
        lc_tools.append(
            StructuredTool.from_function(
                coroutine=descriptor.handler,
                name=descriptor.name,
                description=descriptor.description,
                args_schema=None,  # LangChain will infer from type hints
            )
        )
    return lc_tools


async def run_agent(question: str, mock: bool) -> None:
    async with AgentContext("langchain_agent", mock=mock) as ctx:
        # Store context for the agent session
        await ctx.store("session/question", question)

        try:
            from langchain_anthropic import ChatAnthropic  # type: ignore[import]
            from langchain.agents import AgentExecutor, create_tool_calling_agent
            from langchain_core.prompts import ChatPromptTemplate

            llm = ChatAnthropic(
                model="claude-opus-4-6",
                api_key=os.environ.get("ANTHROPIC_API_KEY", ""),
            )

            lc_tools = _build_langchain_tools()

            prompt = ChatPromptTemplate.from_messages([
                ("system", "You are a helpful agent with access to agentOS tools."),
                ("human", "{input}"),
                ("placeholder", "{agent_scratchpad}"),
            ])

            agent = create_tool_calling_agent(llm, lc_tools, prompt)
            executor = AgentExecutor(agent=agent, tools=lc_tools, verbose=True)
            result = await executor.ainvoke({"input": question})
            print("\nAgent answer:", result["output"])

        except ImportError:
            # LangChain not installed — demonstrate tool dispatch directly
            print("LangChain not installed; running tool dispatch demo instead.")
            search_result = await web_search(query=question, max_results=3)
            print("web_search result:", search_result)

            await ctx.store("notes/answer", str(search_result))
            memories = await ctx.recall(question, k=3)
            print("Recalled:", memories)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="agentOS LangChain agent example")
    parser.add_argument("--mock", action="store_true", default=True)
    parser.add_argument("--question", default="What is agentOS?")
    args = parser.parse_args()
    asyncio.run(run_agent(args.question, args.mock))
