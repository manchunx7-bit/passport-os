"""Build-local workaround for ESP-IDF async connect losing select fd sets.

select() clears sets on timeout. ESP-IDF 5.5.3 initializes them only in INIT,
so the next CONNECTING poll watches no socket. Keep the SDK installation intact.
"""
from pathlib import Path
import sys


def patch(source: str) -> str:
    start = source.index("case ESP_TLS_CONNECTING:")
    end = source.index("if (select(", start)
    block = source[start:end]
    if ("FD_ZERO(&tls->rset)" in block and
            ("FD_SET(tls->sockfd, &tls->wset)" in block or "tls->wset = tls->rset;" in block)):
        return source  # An SDK with the upstream fix needs no workaround.
    anchor = "case ESP_TLS_CONNECTING:\n        if (cfg && cfg->non_block) {\n"
    if source.count(anchor) != 1 or "FD_ZERO" in block or "FD_SET" in block:
        raise ValueError("Unrecognized ESP-TLS connect implementation; review workaround")
    return source.replace(anchor, anchor + """            // select mutates these sets, including when a connect poll times out.
            FD_ZERO(&tls->rset);
            FD_SET(tls->sockfd, &tls->rset);
            tls->wset = tls->rset;
""")


if __name__ == "__main__":
    source, output = map(Path, sys.argv[1:])
    content = patch(source.read_text())
    if not output.exists() or output.read_text() != content:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(content)
