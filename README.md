# OBS AutoFrame v0.2 — Experimental Native Video Filter

OBS AutoFrame is a Windows-targeted OBS Studio filter that uses body detection to digitally pan and zoom around a selected person. This is **experimental**; the Windows DLL build and live OBS compatibility still require validation.

## Features

- Full-body detection: OpenCV HOG fallback; optional YOLOv8/YOLO11 ONNX person detector (model not included)
- Multiple people with tracked IDs and person selection
- Auto-follow, center, leftmost, rightmost, largest, and lock-to-selected-person modes
- Custom subject position, headroom, waist-up/full-body framing, and maximum zoom
- Pan/zoom smoothing, movement limits, dead zone, lead compensation
- Hold last subject or return to wide shot when tracking is lost

This filter applies a **digital crop**, not physical PTZ movement. Full-body HOG detection can be unreliable at distances, during occlusion, and in poorly lit venues.

## Downloading a Windows build (no programming tools on your PC)

1. Open [Actions → Build AutoFrame Windows DLL](https://github.com/newgenstudios12/OBS-autoframe/actions/workflows/build-windows.yml).
2. Choose a run with a **green check mark** (successful). Failed/in-progress runs do not have a validated build.
3. Download the `OBS-AutoFrame-Windows-x64-v0.2` artifact from that run's **Artifacts** section.
4. Close OBS. Extract the artifact to `C:\ProgramData\obs-studio\plugins\obs-autoframe\`, preserving its `bin\64bit\obs-autoframe.dll` layout.
5. Restart OBS. Right-click the camera source → **Filters** → **Effect Filters** → **+** → **AutoFrame (Full-Body Tracker) [Preview]**.
6. If the filter is missing, check **OBS → Help → Log Files → View Current Log** for plugin load errors.

**Important:** A successful CI build verifies Windows compilation and framing unit tests, not actual camera tracking inside OBS. Windows runtime smoke-testing remains necessary before production livestream use.

## Development

The source is in `src/`, the CMake unit test is in `tests/`, and the Windows cloud build is defined in `.github/workflows/build-windows.yml`. To run just the portable logic tests:

```sh
cmake -S . -B build-tests -DAUTOFRAME_BUILD_FILTER=OFF -DBUILD_TESTING=ON
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

The Windows workflow builds OpenCV with vcpkg and integrates with the official [OBS plugin template](https://github.com/obsproject/obs-plugintemplate).

### Origins & licensing

An independent GPL-2.0-or-later implementation informed by [norihiro/obs-face-tracker](https://github.com/norihiro/obs-face-tracker). See `LICENSE`.