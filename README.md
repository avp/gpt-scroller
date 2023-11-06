# sh-gpt-scroller

This is a demo using Static Hermes to build a simple desktop GUI app based on
[DearImGUI](https://github.com/ocornut/imgui).

Originally, this repository contained a simple canvas JS game generated by ChatGPT.
That original source can be found in the [original-js branch](https://github.com/tmikov/gpt-scroller/tree/original-js).
The original game is now one of several windows in the app.

There is a C++ and JS version of the same app, for performance comparison purposes
(the JS version has an extra window, but it can be disabled). The C++ version has
sound effects, which can be disabled either by using the appropriate build
configuration flag or by setting the `NOSOUND` environment variable.

## Building

You need CMake and Ninja (or Make) to build the C++ version.

You need a fork of Static Hermes to build the JS version (we made some changes
in SH for this demo, that we are not sure that we want to land in the official
branch). The fork can be found at https://github.com/tmikov/hermes/tree/sh-gpt-scroller.

There are no other dependencies.

### C++ Version for macOS
```sh
mkdir build
cd build
cmake .. -G Ninja -DSOLOUD_BACKEND_COREAUDIO=ON -DSOLOUD_BACKEND_SDL2=OFF
ninja demo
```

### JS Version for macOS

Assuming you have the forked Static Hermes checked out in $HERMES_SRC and
built in $HERMES_BUILD:

```sh
mkdir build
cd build
cmake .. -G Ninja -DSOLOUD_BACKEND_COREAUDIO=ON -DSOLOUD_BACKEND_SDL2=OFF \
  -DHERMES_BUILD=$HERMES_BUILD -DHERMES_SRC=$HERMES_SRC
ninja jsdemo
```
