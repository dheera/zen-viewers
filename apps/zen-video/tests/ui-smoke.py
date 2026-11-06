#!/usr/bin/env python3
"""Real GTK/mpv pixels and controls. Requires Xvfb, xdotool, xclip, Pillow.
Uses generated Y4M, PCM and SRT fixtures; no codec encoder or network required.
"""
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import time
import wave
from PIL import ImageChops, ImageGrab


def x(*args):
    return subprocess.check_output(['xdotool', *map(str, args)], timeout=8, text=True).strip()


def wait(check, label, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(.05)
    raise AssertionError(label)


def fixture(tmp):
    # BT.601 limited-range quadrants and a moving bar expose flips, wrong channels
    # and stale frames, while raw Y4M is playable without optional codec plugins.
    clip = tmp / 'quadrants.y4m'
    colors = [(81, 90, 240), (145, 54, 34), (41, 240, 110), (210, 16, 146)]
    with clip.open('wb') as out:
        out.write(b'YUV4MPEG2 W96 H64 F12:1 Ip A1:1 C444\n')
        for frame in range(180):
            out.write(b'FRAME\n')
            for channel in range(3):
                plane = bytearray()
                for y in range(64):
                    for x in range(96):
                        value = colors[(y >= 32) * 2 + (x >= 48)][channel]
                        if frame % 80 <= x < frame % 80 + 4:
                            value = (235, 128, 128)[channel]
                        plane.append(value)
                out.write(plane)
    (tmp / 'quadrants.srt').write_text('1\n00:00:00,000 --> 00:00:14,900\nZEN SUBTITLE CHECK\n')
    extra = tmp / 'external.srt'
    extra.write_text('1\n00:00:00,000 --> 00:00:14,900\nEXTERNAL CAPTION\n')
    sound = tmp / 'tone.wav'
    with wave.open(str(sound), 'wb') as out:
        out.setnchannels(1); out.setsampwidth(2); out.setframerate(16000)
        out.writeframes(b''.join(struct.pack('<h', round(300 * math.sin(i * 2 * math.pi * 440 / 16000))) for i in range(16000 * 3)))
    return clip, extra, sound


def run(binary, tmp):
    clip, subtitle, sound = fixture(tmp)
    log = tmp / 'player.log'
    stream = log.open('w')
    app = subprocess.Popen([binary, '--diagnostics', clip], stdout=stream, stderr=stream)
    try:
        win = x('search', '--sync', '--onlyvisible', '--name', 'quadrants.y4m').splitlines()[0]
        x('windowfocus', win)
        x('windowsize', '--sync', win, 600, 500)
        x('mousemove', 1100, 900)
        geo = dict(s.split('=') for s in x('getwindowgeometry', '--shell', win).splitlines())
        ox, oy = int(geo['X']), int(geo['Y'])
        def logs():
            return log.read_text(errors='replace')
        def capture(box=(0, 0, 600, 500)):
            a,b,c,d = box
            return ImageGrab.grab(bbox=(ox+a, oy+b, ox+c, oy+d), xdisplay=os.environ['DISPLAY']).convert('RGB')
        def flag(name, value):
            states = re.findall(r'zen-video ' + name + r'=(\d)', logs())
            return states and int(states[-1]) == value
        def load(path, subtitles=False):
            x('key', 'ctrl+shift+s' if subtitles else 'ctrl+o')
            chooser = x('search', '--sync', '--onlyvisible', '--name', '^Open').splitlines()[-1]
            x('windowfocus', chooser)
            subprocess.run(['xclip', '-selection', 'clipboard', '-i', '-target', 'UTF8_STRING'], input=os.fsencode(path), check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            x('key', 'ctrl+l', 'ctrl+a', 'ctrl+v'); time.sleep(.15); x('key', 'Return')
            wait(lambda: chooser not in subprocess.run(['xdotool','search','--onlyvisible','--name','^Open'], capture_output=True, text=True).stdout.splitlines(), 'Chooser did not close')
            x('windowfocus', win); x('mousemove', 1100, 900)
        wait(lambda: 'zen-video loaded' in logs() and 'zen-video seekable=1' in logs(), 'Load and seekability')
        x('key', 'space'); wait(lambda: flag('pause', 1), 'Pause')
        time.sleep(.6)
        image = capture()
        red, green, blue = image.getpixel((130,150)), image.getpixel((450,150)), image.getpixel((130,330))
        assert red[0] > red[1] + 100 and red[0] > red[2] + 100, ('Top-left red/orientation', red)
        assert green[1] > green[0] + 100 and green[1] > green[2] + 100, ('Top-right green', green)
        assert blue[2] > blue[0] + 100, ('Bottom-left blue', blue)
        assert max(image.getpixel((300,25))) < 45, 'Aspect fit creates top letterbox'
        region = (0, 110, 600, 380)
        before = capture(region); time.sleep(.65); after = capture(region)
        assert ImageChops.difference(before,after).getbbox() is None, 'Paused video should be stable'
        x('key', 'period'); time.sleep(.4)
        assert flag('pause',1), 'Frame step stays paused'
        assert ImageChops.difference(after,capture(region)).getbbox(), 'Frame step changes pixels'
        # Stationary playback requires no frame-clock or polling loop.
        time.sleep(.3)
        def cpu():
            fields = Path('/proc/%d/stat' % app.pid).read_text().rsplit(')',1)[1].split()
            return int(fields[11]) + int(fields[12])
        ticks=cpu(); time.sleep(1.2); idle_ticks=cpu()-ticks
        assert idle_ticks / os.sysconf('SC_CLK_TCK') < .25, ('Paused CPU time', idle_ticks)
        print('PASS orientation, aspect, paused stability, frame step and low idle CPU', flush=True)
        x('key','Home'); time.sleep(.3)
        x('key','Right'); wait(lambda: 'zen-video seek 5 relative' in logs(), 'Relative seek')
        x('key','m'); wait(lambda: flag('mute',1), 'Mute')
        x('key','m'); wait(lambda: flag('mute',0), 'Unmute')
        # Exact sidecar is autoloaded; hiding it must change the bottom image.
        wait(lambda: 'subtitles=1' in logs(), 'Automatic same-name subtitles')
        time.sleep(.4); caption = capture((70,100,530,500))
        if os.environ.get('ZEN_VIDEO_TEST_ARTIFACTS'): capture().save(Path(os.environ['ZEN_VIDEO_TEST_ARTIFACTS']) / 'subtitle-on.png')
        x('key','v'); wait(lambda: flag('sub-visibility',0), 'Hide subtitles'); time.sleep(.3)
        if os.environ.get('ZEN_VIDEO_TEST_ARTIFACTS'): capture().save(Path(os.environ['ZEN_VIDEO_TEST_ARTIFACTS']) / 'subtitle-off.png')
        assert ImageChops.difference(caption,capture((70,100,530,500))).getbbox(), 'Subtitle pixels disappear'
        load(subtitle,subtitles=True)
        wait(lambda: 'subtitle-added' in logs() and 'subtitles=2' in logs(), 'Native subtitle chooser loads external track')
        # Pointer seek should update time without a polling timer.
        x('mousemove','--window',win,300,476); time.sleep(.4)
        previous_seeks = len(re.findall(r'zen-video seek [0-9.]+ absolute\+exact',logs()))
        x('mousemove','--window',win,300,466,'click',1)
        wait(lambda: len(re.findall(r'zen-video seek [0-9.]+ absolute\+exact',logs())) > previous_seeks,'Timeline exact seek')
        x('mousemove',1100,900); x('key','space'); wait(lambda: flag('pause',0),'Resume')
        a=capture(region); time.sleep(.4)
        assert ImageChops.difference(a,capture(region)).getbbox(), 'Playing frames move'
        x('windowsize','--sync',win,340,260); time.sleep(.3)
        x('windowsize','--sync',win,900,700); time.sleep(.3)
        print('PASS keyboard seek, mute, subtitles, timeline, resume and resize',flush=True)
        bad = tmp / 'broken.webm'; bad.write_text('not a media file')
        load(bad); wait(lambda: 'playback-error=' in logs(),'Malformed media error')
        count=logs().count('zen-video loaded')
        load(sound); wait(lambda: logs().count('zen-video loaded')>count and 'tracks video=0 audio=1' in logs(),'Recover to audio file')
        count=logs().count('zen-video loaded')
        load(clip); wait(lambda: logs().count('zen-video loaded')>count,'Recover to video')
        x('key','ctrl+shift+s')
        chooser=x('search','--sync','--onlyvisible','--name','^Open subtitles').splitlines()[-1]
        x('windowfocus',chooser); x('key','Escape'); x('windowfocus',win); x('key','ctrl+w')
        assert app.wait(timeout=8)==0, 'Clean close while playing'
        assert not re.search(r'(CRITICAL|Gtk-WARNING|GLib-WARNING|render-error=)',logs()),logs()
        print('PASS failed-file recovery, audio playback, dialog cancellation and active teardown',flush=True)
    except BaseException:
        print(log.read_text(errors='replace'),file=sys.stderr)
        raise
    finally:
        if app.poll() is None:
            app.terminate()
            try: app.wait(timeout=5)
            except subprocess.TimeoutExpired: app.kill(); app.wait()
        stream.close()


if __name__=='__main__':
    binary=str(Path(sys.argv[1] if len(sys.argv)>1 else 'build/zen-video').resolve())
    if not os.environ.get('ZEN_VIDEO_TEST_DISPLAY'):
        env=dict(os.environ, ZEN_VIDEO_TEST_DISPLAY='1', GDK_BACKEND='x11', GSK_RENDERER='cairo', G_DEBUG='fatal-warnings', GDK_DEBUG='no-portals', GTK_A11Y='none', ZEN_VIDEO_SILENT='1')
        raise SystemExit(subprocess.run(['xvfb-run','-a','-s','-screen 0 1280x1024x24','dbus-run-session','--',sys.executable,__file__,binary],env=env).returncode)
    with tempfile.TemporaryDirectory(prefix='zen-video-ui-') as temp:
        run(binary,Path(temp))
