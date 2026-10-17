#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
plugin = (root / "include/kworker_plugin.h").read_text()
loader = (root / "src/plugin_loader.c").read_text()
main = (root / "src/main.c").read_text()
nady = (root / "plugins/nady_proxy.c").read_text()
makefile = (root / "Makefile").read_text()

assert "KW_PLUGIN_ABI_V2" in plugin
assert 'KW_PLUGIN_ENTRY_SYMBOL "kworker_plugin_v2"' in plugin
assert "worker_index" in plugin
assert "RTLD_NOW | RTLD_LOCAL" in loader
assert "RTLD_NODELETE" not in loader
assert "Kworker %s stopped:" in main
assert "AO server disconnected Kworker" in main
assert "rejected or unexpected AO packet" in main
assert "aocPollArray" in main
assert "Kworker %zu/%zu" in main
config = (root / "src/config.c").read_text()
tell = (root / "plugins/tell.c").read_text()
assert '"[worker]"' in config and '"kworker.conf"' in config
assert 'getenv("AOUSER")' in config
assert "next_send[KW_MAX_WORKERS]" in tell
assert "for (size_t worker_index = 0; worker_index < count; ++worker_index)" in tell
assert "pthread_join" in nady
assert "if (!main_online && !worker_only)" in nady
assert nady.index("if (!main_online && !worker_only)") < nady.index("node = frame_head", nady.index("static int drain_frames"))
assert "Nadybot client socket closed" in nady
assert "Funcom leader socket closed" in nady
assert "select_private_ipv4" in nady
assert "0xc0a80000u" in nady
assert "safe loopback fallback" in nady
assert "listener ready on %s:%s" in nady
assert '"127.0.0.1"' in nady
assert "KW_BUDDY_SERVICE" in nady and "KW_TELL_SERVICE" in nady
assert "tell_round_robin" in nady
assert "worker_buddy_count[KW_MAX_WORKERS]" in nady
assert "mysql" not in "\n".join(
    p.read_text(errors="ignore").lower()
    for p in list((root / "src").glob("*.c")) + list((root / "plugins").glob("*.c"))
)
print("kworker policy checks passed")
