from cchud_daemon.registry import AdapterRegistry
from cchud_daemon.adapters.claude import ClaudeAdapter


def test_routes_to_claude():
    r = AdapterRegistry()
    r.register(ClaudeAdapter())
    evs = r.normalize({"client": "claude", "event": "Stop",
                       "payload": {"session_id": "s1"}})
    assert evs and evs[0].state == "idle"


def test_unknown_client_returns_empty():
    assert AdapterRegistry().normalize({"client": "nope", "event": "x"}) == []
