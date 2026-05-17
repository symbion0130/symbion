"""End-to-end MCP check: spawn the time server, discover tools, dispatch
one call, shut down. Used to validate MCPManager wiring without booting
the full web app.

Run after `pip install mcp mcp-server-time`.
"""
import sys, asyncio
sys.path.insert(0, ".")

from symbion_v14 import SymbionConfig, SYMBION, _MCP


async def main() -> int:
    if not _MCP:
        print("FAIL: mcp SDK not importable. Run: pip install mcp")
        return 1

    cfg = SymbionConfig()
    cfg.tools_enabled = False
    cfg.self_eval_enabled = False
    cfg.mcp_enabled = True
    cfg.mcp_servers = [{
        "name": "time",
        "command": sys.executable,
        "args": ["-m", "mcp_server_time"],
        "enabled": True,
    }]

    symbion = SYMBION(cfg)
    await symbion.start_mcp()

    if not symbion.mcp.started:
        print("FAIL: MCP did not start")
        await symbion.stop_mcp()
        return 1

    tools = symbion.mcp.tool_schemas()
    print(f"Discovered {len(tools)} tool(s):")
    for t in tools:
        print(f"  - {t['name']}  ({t['description'][:80]})")

    if not tools:
        print("FAIL: no tools discovered")
        await symbion.stop_mcp()
        return 1

    # Find get_current_time and call it. Tool name is exposed via the
    # original Tool object; we know the time server emits get_current_time
    # so dispatch directly on the qualified name.
    qname = next((t["name"] for t in tools if "current_time" in t["name"]), tools[0]["name"])
    print(f"\nCalling {qname}...")
    result = await symbion.mcp.dispatch(qname, {"timezone": "America/New_York"})
    print("Result:")
    print("  " + result.replace("\n", "\n  ")[:600])

    print("\nShutting down...")
    await symbion.stop_mcp()
    print("OK — MCP check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
