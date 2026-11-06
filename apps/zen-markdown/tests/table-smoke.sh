#!/usr/bin/env bash
# Optional X11 integration check: xvfb, xauth, xdotool, xclip, dbus-daemon, python3.
set -euo pipefail
app=$(realpath "${1:-build/zen-markdown}")
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
cat > "$tmp/table-check.md" <<'MARKDOWN'
| Name | Style | Link |
| :--- | :---: | ---: |
| naïve | **bold** and `a\|b` | [Jump](#chapter) |

## Chapter

Paragraph after the table.

```text
first code line

last code line
```
MARKDOWN
xvfb-run -a -s '-screen 0 1280x1024x24' dbus-run-session -- env GTK_A11Y=none GDK_BACKEND=x11 \
    GSK_RENDERER=cairo GSETTINGS_BACKEND=memory G_DEBUG=fatal-warnings python3 - "$app" "$tmp/table-check.md" <<'PY'
import pathlib, subprocess, sys, time
app = subprocess.Popen([sys.argv[1],sys.argv[2]])
def x(*cmd):
    return subprocess.check_output(["xdotool",*map(str,cmd)],text=True,timeout=8).strip()
def clip():
    return subprocess.check_output(["xclip","-selection","clipboard","-out","-target","UTF8_STRING"],text=True,timeout=3)
try:
    wid=x("search","--sync","--onlyvisible","--pid",app.pid,"--name","table-check.md").splitlines()[0]
    x("windowfocus","--sync",wid)
    time.sleep(.3)
    x("mousemove","--window",wid,740,161,"click",1)
    expected="Name\tStyle\tLink\nnaïve\tbold and a|b\tJump\n"
    assert clip()==expected,repr(clip())
    print("PASS whole-table TSV clipboard")
    x("mousemove","--window",wid,58,125,"click","--repeat",2,"--delay",100,1,"key","ctrl+c")
    assert clip()=="naïve",repr(clip())
    print("PASS selectable cell and native clipboard")
    x("mousemove","--window",wid,741,125,"click",1,"key","shift+End","ctrl+c")
    assert clip()=="Chapter",repr(clip())
    print("PASS inline table link to document heading")
    x("key","ctrl+a","ctrl+c")
    rendered=clip()
    assert expected.rstrip()+"\n\nChapter" in rendered and "\ufffc" not in rendered,repr(rendered)
    assert "first code line\n\nlast code line" in rendered
    print("PASS document copy includes table cells and exact code whitespace")
    x("windowsize",wid,420,700)
    time.sleep(.2)
    geometry=dict(line.split("=") for line in x("getwindowgeometry","--shell",wid).splitlines())
    assert geometry["WIDTH"]=="420",geometry
    x("key","ctrl+plus","ctrl+plus","ctrl+a","ctrl+c")
    assert clip()==rendered
    x("key","ctrl+u","ctrl+a","ctrl+c")
    assert clip()==pathlib.Path(sys.argv[2]).read_text()
    x("key","ctrl+u","ctrl+0","ctrl+a","ctrl+c")
    assert clip()==rendered
    print("PASS narrow-window resize, zoom, and lossless source toggle")
    x("key","ctrl+w")
    assert app.wait(timeout=4)==0
finally:
    if app.poll() is None:
        app.kill()
        app.wait()
PY
