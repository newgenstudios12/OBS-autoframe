# OBS AutoFrame — Windows DLL cloud-build source kit (v0.2)

**This ZIP is not a compiled Windows DLL.** It adds a GitHub Actions build workflow to our C++ OBS filter source, so you can build on GitHub's Windows runner without installing Python, C++, Visual Studio or OpenCV on your PC. The Windows workflow has **not been executed or validated** yet; the source-only C++ tests have passed on Linux.

## Build without installing developer tools

1. Sign into GitHub, create a new repository (e.g., `obs-autoframe`) and select **Add file → Upload files**. Extract this ZIP first, then upload its contents **including the `.github` directory**. GitHub hides folders beginning with a dot on some file pickers: use drag-and-drop for the complete extracted directory contents, or use GitHub Desktop.
2. Commit the files to the default branch. Open **Actions → Build AutoFrame Windows DLL → Run workflow** if the workflow did not run automatically. This uses a **Windows 2022 hosted runner** and may take a while (OpenCV/OBS dependency compilation).
3. If the action is green, open the completed build and download the `OBS-AutoFrame-Windows-x64-v0.2` artifact ZIP. Do not download the source ZIP as a substitute for this artifact.
4. Shut down OBS. Extract the artifact into `C:\ProgramData\obs-studio\plugins\obs-autoframe\`, ensuring `obs-autoframe.dll` is under `bin\64bit\`. Restart OBS. Add the **AutoFrame (Full-Body Tracker) [Preview]** effect filter to a camera source.
5. If the filter doesn't appear or OBS reports a missing DLL, use **Help → Log Files → View Current Log**. This build hasn't undergone an OBS runtime/load test, and additional runtime dependencies or code fixes may be necessary.

## What the workflow tries

- Compile and execute the independent C++ framing/selection tests **on Windows**.
- Obtain OBS's maintained Windows plugin template and build OBS's development dependencies.
- Install the OpenCV 4 C++ libraries in a static x64 triplet with vcpkg, compile `src/plugin.cpp` into a native Windows OBS filter.
- Check that the DLL has an MZ/PE header, then upload a native OBS plugin directory tree as a downloadable workflow artifact.

**Limitations:** A green CI build validates compilation and logic tests, not a live OBS instance. The OBS plugin template's master branch, OBS SDK dependencies, and vcpkg can evolve and may require build fixes. This is an *experimental build workflow*, not a guaranteed prebuilt download or tested plugin. The ONNX detector model is not bundled. The built-in HOG detector is only a limited fallback. The plugin digitally reframes a camera and does not physically pan a camera.

Official OBS plugin template: https://github.com/obsproject/obs-plugintemplate
Official OBS manual installation guidance: https://obsproject.com/kb/plugins-guide