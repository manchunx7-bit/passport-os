"""Plugin policy lives in descriptors, not in per-app switches."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
registry = (root / "main/launcher/app_registry.cc").read_text(encoding="utf-8")
header = (root / "main/launcher/app_registry.h").read_text(encoding="utf-8")
base = (root / "main/apps/app_base.h").read_text(encoding="utf-8")
settings = (root / "main/launcher/settings.cc").read_text(encoding="utf-8")
mcp = (root / "main/device_control_mcp.cc").read_text(encoding="utf-8")

for field in ("key", "in_launcher", "openable", "optional", "default_enabled",
              "allow_toast", "streaming", "heap_largest_kb", "heap_free_kb"):
    assert re.search(rf"\b{field};", base), f"missing plugin metadata: {field}"

# One link-time table, including the system screens.
table = registry.split("const passport_app_t *s_apps[] = {", 1)[1].split("};", 1)[0]
registered = set(re.findall(r"&g_(\w+)_app", table))
assert {"home", "wifi"} <= registered, "system screens must be registered too"

# Behaviour differences come from metadata.
for signature, field in (("bool feedback_allowed(", "allow_toast"),
                         ("WifiPowerSaveLevel power_save_level_for(", "streaming"),
                         ("HeapNeed heap_need(", "heap_largest_kb")):
    start = registry.index(signature)
    body = registry[start:registry.index("\n}", start)]
    assert f"app->{field}" in body, f"{signature} must read {field}"
    assert "switch (id)" not in body, f"{signature} must not switch on app id"

# Visibility is computed from the descriptor flags and the enabled table.
for marker in ("app->in_launcher && s_enabled[app->id]",
               "app->openable && s_enabled[app->id]",
               'nvs_open(kFlagsNamespace', "nvs_set_u8(handle, app->key"):
    assert marker in registry, f"registry missing plugin marker: {marker}"

# Every descriptor declares a stable key; only voice and Muse are optional
# and both default off.
descriptors = {}
for path in (root / "main").rglob("*.cc"):
    for match in re.finditer(r"const passport_app_t g_(\w+)_app\s*=\s*\{([\s\S]+?)\};",
                             path.read_text(encoding="utf-8")):
        descriptors[match[1]] = match[2]
assert len(descriptors) >= 15, descriptors.keys()
for name, body in descriptors.items():
    assert re.search(r'\.key\s*=\s*"[a-z_]+"', body), f"g_{name}_app has no key"
optional = {name for name, body in descriptors.items()
            if re.search(r"\.optional\s*=\s*true", body)}
assert optional == {"voice", "muse"}, optional
for name in optional:
    assert re.search(r"\.default_enabled\s*=\s*false", descriptors[name]), name

# Settings generates toggle rows from the registry and resets them with defaults.
for marker in ("app_registry_optional_count()", "app_registry_get_optional(",
               "app_registry_set_enabled(", "app_registry_reset_optional()"):
    assert marker in settings, f"settings missing plugin marker: {marker}"
assert "kRowCount" not in settings, "settings row count must follow the registry"

# Device control derives the open-app schema and key mapping from the registry.
assert "app_registry_openable_count()" in mcp and "app_registry_get_openable(" in mcp
assert "constexpr App apps[]" not in mcp and "app_key(" not in mcp
assert "app_registry_key(" in mcp
assert "app_registry_set_enabled" in header and "app_registry_enabled" in header

print("Plugins: descriptor-driven policy, optional app persistence and derived "
      "settings/MCP surfaces: PASS")
