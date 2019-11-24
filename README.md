# GLDosbox

This is a branch of dosbox version 0.74-3 which is intended to add more sophisticated OpenGL and shader support, while removing legacy dependencies and unnecessary functionality for modern PCs.
Please note that this version of Dosbox has not been tested thoroughly!
See [AUTHORS](https://github.com/BasFaggingerAuer/dosbox-staging/blob/gldosbox/AUTHORS) for a list of all contributors.

![GLDosbox with Timothy Lottes' CRT shader.](https://github.com/BasFaggingerAuer/dosbox-staging/blob/gldosbox/dn1.png)

## Added functionality

* SDL2 compatibility.
* OpenGL 3.3 core compatibility.
* OpenGL 3.3 shader support.
* Roland MT-32 support via [MUNT](https://github.com/munt/munt).

## Removed functionality

* Legacy SDL 1.2.x compatibility.
* Support for physical CD-ROM drives.
* Legacy SDL Sound support.
* Non-scancode keymapping.
* Switcheable fullscreen mode.
* WinDIB.
* Software scalers.
* Non-OpenGL rendering modes (surface, overlay, ...).

## Compatibility

Currently this version has been tested only on Ubuntu 18.04.
OS2/Windows/Mac compatibility is likely broken.

## Installation

Please ensure the following libraries are available:
* Sane Linux/Unix build environment.
* SDL version >= 2.0.0.
* OpenGL.
* GLEW version >= 1.5.4.
The following dependencies are optional:
* Curses.
* PNG library.
* SDL net >= 2.0.0.
* [MUNT](https://github.com/munt/munt).

On Debian/Ubuntu, these dependencies can be installed via
```
sudo apt install build-essential autotools-dev libsdl2-dev libsdl2-net-dev libglew-dev libpng-dev
```

To compile, issue the following commands,
```
./autogen.sh
./configure
make
sudo make install
```

The emulator can then be run using
```
gldosbox
```

## Configuration and shaders

Configuration files are very similar to those of Dosbox and stored in
```
~/.dosbox/gldosbox-0.74-3.conf
```
this file will be auto-generated if it does not exist.

Example vertex and fragment shaders are provided in
```
src/default.vert
src/default.frag
src/crt_lottes.frag
```
where last is a very much recommended CRT shader created by Timothy Lottes.

To enable the shaders, provide their full path in the configuration file, e.g.,
```
...
vertexshader=/home/yourusername/gldosbox/src/default.vert
fragmentshader=/home/yourusername/gldosbox/src/crt_lottes.frag
...
```

To enable Roland MT-32 emulation, the appropriate ROMS must be referenced in the configuration file, e.g.,
```
...
mt32.romdir=/where/you/store/the/roland/roms
...
```
see the comments in the configuration file for more information.

