#!/bin/bash
set -euo pipefail

MODEL="qwen3.5:35b-a3b-q4_K_M"

usage() {
    echo "Usage: $0 [--model MODEL] <filename>"
}

if [ "${1:-}" = "--model" ]; then
    if [ $# -lt 3 ]; then
        usage
        exit 1
    fi
    MODEL="$2"
    shift 2
fi

if [ $# -lt 1 ]; then
    usage
    exit 1
fi

IMG="$1"
if [ ! -f "$IMG" ]; then
    echo "Error: file '$IMG' not found"
    exit 1
fi

if ! command -v ollama >/dev/null 2>&1; then
    echo "Error: 'ollama' not found in PATH"
    exit 1
fi

# Verify model availability explicitly to avoid opaque API failures.
if ! ollama list | awk 'NR>1 {print $1}' | grep -Fxq "$MODEL"; then
    echo "Error: model '$MODEL' not found in local Ollama registry."
    echo "Installed models:"
    ollama list
    exit 1
fi

python3 - "$IMG" "$MODEL" <<'PY'
import base64
import json
import sys
import urllib.error
import urllib.request

img_path = sys.argv[1]
model = sys.argv[2]

with open(img_path, "rb") as f:
    img_b64 = base64.b64encode(f.read()).decode("ascii")

payload = {
    "model": model,
    "messages": [
        {
            "role": "user",
            "content": (
                "Describe this screenshot in a short structured report: "
                "SPRITES (what, where, color), TEXT (content, position), "
                "BACKGROUND (platforms, structures), GLITCHES (artifacts, "
                "misalignment, wrong colors, spurious pixels, or nothing if "
                "everything looks ok). Skip sections with nothing to report. "
                "Be terse: one line per element. Use approximate pixel "
                "positions (320x200 screen)."
            ),
            "images": [img_b64],
        }
    ],
    "think": False,
    "stream": False,
}

req = urllib.request.Request(
    "http://localhost:11434/api/chat",
    data=json.dumps(payload).encode("utf-8"),
    headers={"Content-Type": "application/json"},
)

try:
    with urllib.request.urlopen(req, timeout=240) as resp:
        raw = resp.read().decode("utf-8", errors="replace")
except urllib.error.HTTPError as e:
    body = e.read().decode("utf-8", errors="replace")
    print(f"Error: Ollama API returned HTTP {e.code}", file=sys.stderr)
    if body:
        print(body, file=sys.stderr)
    sys.exit(1)
except Exception as e:
    print(f"Error: failed to call Ollama API: {e}", file=sys.stderr)
    sys.exit(1)

try:
    obj = json.loads(raw)
except json.JSONDecodeError:
    print("Error: Ollama API returned non-JSON output:", file=sys.stderr)
    print(raw, file=sys.stderr)
    sys.exit(1)

if "error" in obj:
    print(f"Error: {obj['error']}", file=sys.stderr)
    sys.exit(1)

msg = obj.get("message", {}).get("content")
if not msg:
    print("Error: unexpected Ollama response (missing message.content):", file=sys.stderr)
    print(json.dumps(obj, indent=2), file=sys.stderr)
    sys.exit(1)

print(msg)
PY
