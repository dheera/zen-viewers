#!/usr/bin/env bash
# Optional X11 integration check: xvfb, xauth, xdotool, xclip, dbus-daemon, python3.
set -euo pipefail
app=$(realpath "${1:-build/zen-markdown}")
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
cat > "$tmp/reading.md" <<'MARKDOWN'
[Jump to chapter](#chapter)

# Reading test

A **bold word** beside a [local anchor](#chapter).

1. First item
2. Second item

## Chapter

Inline `code` and *emphasis*.
MARKDOWN
xvfb-run -a -s '-screen 0 1280x1024x24' dbus-run-session -- env GTK_A11Y=none GDK_BACKEND=x11 \
    GSK_RENDERER=cairo GSETTINGS_BACKEND=memory G_DEBUG=fatal-warnings python3 - "$app" "$tmp/reading.md" <<'PY'
import pathlib, subprocess, sys, time
app = subprocess.Popen([sys.argv[1],sys.argv[2]])
def x(*cmd):
    return subprocess.check_output(["xdotool",*map(str,cmd)],text=True,timeout=8).strip()
def clip():
    return subprocess.check_output(["xclip","-selection","clipboard","-out","-target","UTF8_STRING"],text=True,timeout=3)
try:
    wid=x("search","--sync","--onlyvisible","--pid",app.pid,"--name","reading.md").splitlines()[0]
    x("windowfocus","--sync",wid)
    x("mousemove","--window",wid,48,67,"click",1)
    x("key","shift+End","ctrl+c")
    assert clip()=="Chapter"
    print("PASS click a local heading link")
    x("key","ctrl+a","ctrl+c")
    text=clip()
    assert "A bold word beside a local anchor." in text and "1.  First item" in text and "#" not in text,repr(text)
    print("PASS formatted reading order and native selected-text clipboard")
    x("key","ctrl+u","ctrl+a","ctrl+c")
    assert clip()==pathlib.Path(sys.argv[2]).read_text()
    print("PASS source toggle and exact source clipboard")
    x("key","ctrl+u","ctrl+plus","ctrl+0","ctrl+a","ctrl+c")
    assert clip()==text
    print("PASS rendered view after zoom and reset")
    x("key","ctrl+w")
    assert app.wait(timeout=4)==0
finally:
    if app.poll() is None:
        app.kill()
        app.wait()
PY
