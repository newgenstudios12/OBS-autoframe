# OBS AutoFrame

Native OBS Studio video filter for person tracking, subject selection, and smooth digital reframing.

> Experimental: Windows DLL compilation and live OBS compatibility have not yet been verified. The Windows build workflow will publish test artifacts if compilation succeeds.

Features planned for v0.2: full-body detection (OpenCV HOG or optional YOLO ONNX), multi-person target selection, adjustable framing, smoothing, and hold/return-wide behavior.

Builds: see **Actions → Build AutoFrame Windows DLL** once the workflow is uploaded.

Based on a separate implementation informed by [obs-face-tracker](https://github.com/norihiro/obs-face-tracker). GPL-2.0-or-later.
