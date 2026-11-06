# zen-video

A focused local video player using GTK 4 and libmpv. Video fills the window while preserving its aspect ratio, with dark margins where needed. Move near the top edge for file and window controls, or the bottom edge for playback, the timeline, time, mute and subtitles. The last frame remains visible when a video finishes; press Space to replay it.

```sh
zen-video movie.mp4
zen-video --diagnostics movie.webm
```

| Action | Control |
| --- | --- |
| Play / pause, or replay after the end | **Space**, **K**, or the play button |
| Seek backward / forward | **Left** / **Right**: 5 seconds; **Shift+Left** / **Shift+Right**: 30 seconds |
| Jump to a position | Drag or click the bottom timeline |
| Return to the beginning | **Home** |
| Change volume | **Up** / **Down**: 5 percentage points |
| Mute / unmute | **M**, or the speaker button |
| Advance / go back one frame | **.** / **,**; leaves playback paused |
| Cycle subtitle tracks | **J**, or the subtitle menu |
| Hide / show subtitles | **V**, or the subtitle menu |
| Open an external subtitle | **Ctrl+Shift+S**, the subtitle menu, or drop an SRT/VTT/ASS/SSA/SUB file |
| Fullscreen | **F**, **F11**, double-click the video, or the fullscreen button |
| Leave fullscreen | **Esc** |
| Open another local media file | **Ctrl+O**, or drop it into the window |
| Close the window | **Ctrl+W** |

Embedded subtitles and exact matching sidecars, such as `movie.mp4` with `movie.srt`, are available automatically. libmpv renders the subtitle text, including ASS styling. Stepping backward or seeking in compressed video can require decoding from an earlier keyframe. Timeline seeking is available when the media reports that it is seekable. Audio-only files also play, with a quiet placeholder in place of the video.

The player uses libmpv's OpenGL render API inside `GtkGLArea`. Decoding and media I/O run in libmpv's worker threads. GTK redraws when a video frame or control changes; paused playback has no continuous render or polling timer. The UI uses asynchronous mpv commands and coalesces timeline seeks during dragging.

Hardware decoding is requested with `hwdec=auto-safe`, falling back to CPU decoding when the installed codec or driver cannot use it. GPU presentation and hardware decoding are separate: a video can use the GPU for scaling and presentation while decoding on the CPU. Mesa OpenGL works across supported Intel and AMD hardware; NVIDIA uses the system's OpenGL and media drivers. An OpenGL 3.3 context is required, including Mesa software OpenGL when acceleration is unavailable. Formats and hardware acceleration depend on the system libmpv/FFmpeg build and drivers. See the [mpv manual](https://mpv.io/manual/stable/) for backend details.

Audio tries PulseAudio (including PipeWire's PulseAudio service), then ALSA. If neither is available, playback continues silently and the window shows a notice. `--diagnostics` prints the GL renderer, selected audio output, decoder status, playback events and backend warnings. Codec or driver warnings can accompany a successful fallback. The app does not load user mpv configuration or scripts. It opens local files rather than accepting network URLs. Indirect references and the youtube-dl hook are disabled; linked MOV media, playlists and ordered chapters spanning separate files are not supported. Matching subtitle files still load normally.

Build this app from the repository root on Ubuntu 22.04 or newer:

```sh
sudo apt install build-essential pkg-config libgtk-4-dev libmpv-dev libepoxy-dev
make zen-video
build/zen-video examples/basin.webm
```

Only the installed Zen Video package needs libmpv at runtime; other Zen Viewers do not acquire that dependency. Package dependencies use the libmpv ABI supplied by the distribution.

The optional UI test uses a private Xvfb display and generates raw video, audio and subtitle fixtures without an encoder or network access. It verifies actual displayed colors and orientation, aspect fitting, paused pixel stability and low CPU use, frame stepping, seeking, mute, subtitle loading, resizing, failed-file recovery and closing during playback.

```sh
sudo apt install xvfb xauth xdotool xclip dbus-daemon python3-pil libgl1-mesa-dri
python3 apps/zen-video/tests/ui-smoke.py build/zen-video
```

`ZEN_VIDEO_SILENT=1` deliberately selects silent audio output for automated tests. Set `ZEN_VIDEO_TEST_ARTIFACTS` to an existing directory to save the test's subtitle screenshots.
