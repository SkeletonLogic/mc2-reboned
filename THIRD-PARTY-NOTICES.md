# Third-party notices

This repository contains no third-party source beyond what Microsoft's MechCommander 2
release already carried. The build fetches and links the libraries below from their own
upstreams (see the README). Their licence texts are in [`licenses/`](licenses/).

## Dawn (WebGPU implementation)

- Upstream: <https://dawn.googlesource.com/dawn>
- Used by: the Windows renderer (static link) and the browser build (`emdawnwebgpu`).
- Licence: BSD 3-Clause, with the notices for Dawn's own bundled components --
  [`licenses/Dawn-BSD-3-Clause.txt`](licenses/Dawn-BSD-3-Clause.txt).

## zlib

- Upstream: <https://github.com/madler/zlib>, tag `v1.3.1`, compiled into the game.
- Microsoft's original GameOS headers in `src/gameos/include/` (`zlib.h`, `zconf.h`,
  `zutil.h`, `deflate.h`) are zlib 1.1.x headers by Jean-loup Gailly and Mark Adler,
  shipped as part of Microsoft's release; their notices are retained in those files.
- Licence: zlib licence -- [`licenses/zlib.txt`](licenses/zlib.txt).

## FFmpeg

- Upstream: <https://ffmpeg.org/> (source: <https://git.ffmpeg.org/ffmpeg.git>).
- Used by: the optional Windows video decoder (`MC2_ENABLE_FFMPEG`), dynamically linked
  against the shared libraries `avcodec`, `avformat`, `avutil`, `swresample`, `swscale`.
- The project uses an **LGPL** build of FFmpeg 7.1 (configured with `--enable-version3`,
  without `--enable-gpl` or `--enable-nonfree`), i.e. FFmpeg under the GNU Lesser General
  Public License version 3 or later -- [`licenses/LGPL-3.0.txt`](licenses/LGPL-3.0.txt).
  The LGPL v3 incorporates the GNU GPL v3: <https://www.gnu.org/licenses/gpl-3.0.txt>.
- FFmpeg is not modified. Because it is dynamically linked, anyone may replace the
  FFmpeg DLLs next to `MechCmd2.exe` with their own compatible build.

## Microsoft and FASA

MechCommander 2 was developed by FASA Interactive and published by Microsoft. All
Microsoft and FASA copyright notices in the source are retained, as the licence requires.
The source also carries notices from Virtual World Entertainment and Non-Linear Arts, Inc.
that were present in Microsoft's release; they are retained unchanged.
