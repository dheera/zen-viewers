#!/usr/bin/env python3
"""Native parser regressions using original, temporary ZIP fixtures only."""
import http.server
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import threading
import zipfile
import zlib

HERE = Path(__file__).resolve().parent


def image():
    def chunk(kind, data):
        return struct.pack(">I", len(data))+kind+data+struct.pack(">I", zlib.crc32(kind+data))
    return b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0))+chunk(b"IDAT", zlib.compress(b"\0\x22\x66\x88"))+chunk(b"IEND", b"")


def epub(path, body=None, ncx=False, extras=None):
    container = '<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OPS/book.opf"/></rootfiles></container>'
    navigation = '<nav xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" epub:type="toc"><ol><li><a href="one.xhtml#start">First chapter</a><ol><li><a href="two%20%231.xhtml#目标">Second chapter</a></li></ol></li></ol></nav>'
    legacy = '<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/"><navMap><navPoint><navLabel><text>First chapter</text></navLabel><content src="one.xhtml#start"/><navPoint><navLabel><text>Second chapter</text></navLabel><content src="two%20%231.xhtml#目标"/></navPoint></navPoint></navMap></ncx>'
    opf = f'''<package xmlns="http://www.idpf.org/2007/opf" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Café 中文</dc:title><dc:creator>Zoë Author</dc:creator></metadata><manifest>
    <item id="a" href="one.xhtml" media-type="application/xhtml+xml"/><item id="b" href="two%20%231.xhtml" media-type="application/xhtml+xml"/>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" {'properties="nav"' if not ncx else ''}/><item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
    <item id="pic" href="images/pixel.png" media-type="image/png"/></manifest><spine toc="ncx"><itemref idref="a"/><itemref idref="b"/></spine></package>'''
    default = '''<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Chapter one</title><style>bad-css-text</style><script>bad-script-text</script></head><body><h1 id="start">Résumé 中文</h1><p>café <strong>naïve ☃</strong> <em>αβ</em> <code>π()</code> <a href="two%20%231.xhtml#目标">next</a></p><blockquote>A quotation.</blockquote><ol><li>One</li><li>Two</li></ol><pre>line one\n  line two</pre><img src="images/pixel.png" alt="Pixel"/><svg xmlns="http://www.w3.org/2000/svg"><image href="images/pixel.png"/></svg><p hidden="hidden">hidden-content</p><a href="javascript:alert(1)">inert</a><a href="https://example.invalid/">external</a><a href="../../outside.xhtml">escape</a><script>bad-script-text</script></body></html>'''
    resources = {"mimetype": b"application/epub+zip", "META-INF/container.xml": container,
                 "OPS/book.opf": opf, "OPS/nav.xhtml": navigation, "OPS/toc.ncx": legacy,
                 "OPS/one.xhtml": body if body is not None else default,
                 "OPS/two #1.xhtml": '<html xmlns="http://www.w3.org/1999/xhtml"><body><h2 id="目标">The destination</h2><a href="one.xhtml#start">back</a></body></html>',
                 "OPS/images/pixel.png": image()}
    if extras:
        resources.update(extras)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as book:
        for name, data in resources.items():
            book.writestr(name, data)


class NetworkProbe(http.server.BaseHTTPRequestHandler):
    requests = 0
    def do_GET(self):
        type(self).requests += 1
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"external network content")
    def log_message(self, *args):
        pass


def main():
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "libarchive", "libxml-2.0", "gio-2.0"], text=True))
    with tempfile.TemporaryDirectory(prefix="zen-book-parser-") as temp:
        tmp = Path(temp)
        executable = tmp/"check"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I"+str(HERE.parent/"src"), str(HERE/"book-check.c"), str(HERE.parent/"src/book.c"), "-o", str(executable), *flags], check=True)
        def check(name, mode="dump", okay=True, resource=None):
            args = [str(executable), mode, str(tmp/name)] + ([resource] if resource is not None else [])
            result = subprocess.run(args, capture_output=True, text=True, timeout=15)
            if okay:
                assert result.returncode == 0, (name, result.stderr)
                return json.loads(result.stdout) if mode in ("dump", "metadata") else result.stdout
            assert result.returncode == 1 and result.stderr, (name, result)
            return result
        epub(tmp/"book.epub")
        book = check("book.epub")
        assert book["title"] == "Café 中文" and book["author"] == "Zoë Author" and book["kind"] == 0, book
        assert [c["title"] for c in book["chapters"]] == ["First chapter", "Second chapter"], book
        assert [e["depth"] for e in book["toc"]] == [0, 1], book
        first = book["chapters"][0]
        assert "1. One" in first["text"] and "2. Two" in first["text"] and "line one\n  line two" in first["text"], first
        assert any(first["text"][a:b].strip() == "naïve ☃" and flags & 1 for a,b,flags,heading in first["spans"]), first
        assert any(link["chapter"] == 1 and link["fragment"] == "目标" for link in first["links"]), first
        assert len(first["images"]) == 2 and all(i["path"] == "OPS/images/pixel.png" for i in first["images"]), first
        assert "bad-script-text" not in first["text"] and "hidden-content" not in first["text"] and "bad-css-text" not in first["text"]
        assert not any(link["target"].startswith(("javascript:", "../")) for link in first["links"])
        assert int(check("book.epub", "resource", resource="OPS/images/pixel.png")) == len(image())
        check("book.epub", "resource", okay=False, resource="../outside.txt")
        check("book.epub", "threads")
        epub(tmp/"percent.epub", body='<html><body><a href="#percent%25anchor">jump</a><p id="percent%anchor">destination</p></body></html>')
        percent = check("percent.epub")["chapters"][0]
        assert percent["links"][0]["fragment"] == "percent%anchor", percent
        epub(tmp/"legacy.epub", ncx=True)
        assert len(check("legacy.epub")["toc"]) == 2
        print("PASS EPUB3 nav / EPUB2 NCX, Unicode offsets/styles, local links/fragments/images, script exclusion and concurrent reads")

        with zipfile.ZipFile(tmp/"comic.cbz", "w") as comic:
            for name in ("page10.PNG", "page2.png", "page1.jpg", "__MACOSX/._page0.png"):
                comic.writestr(name, image())
            comic.writestr("notes.txt", "not an image")
        comic = check("comic.cbz")
        assert comic["kind"] == 1 and [c["path"] for c in comic["chapters"]] == ["page1.jpg", "page2.png", "page10.PNG"], comic
        assert all(c["text"] == "\ufffc" and len(c["images"]) == 1 for c in comic["chapters"])
        with zipfile.ZipFile(tmp/"lazy.cbz", "w", zipfile.ZIP_DEFLATED) as comic:
            comic.writestr("page1.png", b"not decoded during metadata opening")
        assert len(check("lazy.cbz", "metadata")["chapters"]) == 1
        print("PASS natural CBZ page order, lazy resources and cancellation")

        for name in ("../escape", "/absolute", "folder/../../escape"):
            epub(tmp/"unsafe.epub", extras={name:"untrusted"})
            check("unsafe.epub", okay=False)
        secret = tmp/"private.txt"
        secret.write_text("ENTITY-SECRET-MUST-NOT-RENDER")
        malicious = f'<!DOCTYPE html [<!ENTITY leak SYSTEM "{secret.as_uri()}">]><html><body>&leak;</body></html>'
        epub(tmp/"entity.epub", body=malicious)
        result = check("entity.epub", okay=False)
        assert "ENTITY-SECRET" not in result.stdout
        epub(tmp/"huge.epub", body="<html><body>"+"x"*(2*1024*1024+1)+"</body></html>")
        check("huge.epub", okay=False)
        (tmp/"broken.cbz").write_bytes(b"not a ZIP")
        check("broken.cbz", okay=False)
        with http.server.ThreadingHTTPServer(("127.0.0.1", 0), NetworkProbe) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            url = f"http://127.0.0.1:{server.server_address[1]}/external"
            epub(tmp/"offline.epub", body=f'<!DOCTYPE html SYSTEM "{url}"><html><body>Offline text<img src="{url}" alt="remote image"/><iframe src="{url}"/><script>fetch("{url}")</script></body></html>')
            assert "Offline text" in check("offline.epub")["chapters"][0]["text"]
            server.shutdown(); thread.join()
            assert NetworkProbe.requests == 0, "Book parsing attempted a network request"
        print("PASS unsafe ZIP paths, malformed archives, XML entity rejection, text bounds and offline parsing")


if __name__ == "__main__":
    main()
