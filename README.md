picom
=====

__picom__ is a compositor for X, and a [fork of Compton](History.md).

**This is a development branch, bugs to be expected**

You can leave your feedback or thoughts in the [discussion tab](https://github.com/yshui/picom/discussions).

## About this Fork

Extended animation support (auto, fly-in, zoom, squeeze, slide-up, slide-down, slide-left, slide-right) from original [repo](https://github.com/dccsillag/picom) by @dccsillag, added slide-out, slide-in for workspace switching with [Custom Fluxbox](https://github.com/pijulius/fluxbox/commit/83ee4dbee67a68fe38b0f96431473719e165bf1e) , animation on window type basis (for e.g. make transient windows like "about us" slide down and slide backup to the titlebar of the owner window), animation-exclude list for disabling animations on window/program basis and some fixes.

Reason: original picom by @yshui [repo](https://github.com/yshui/picom/) didn't have animation support and @dccsillag implementation was the best could find so building uppon his work this repo was made. FYI: looks like original picom will have animation support starting with version 12 but will keep this repo as atm looks like the animation here are smoother/better (of course everyone's own point of view) and had some trouble with the latest picom so till find out the reasons will use this repo personally and everyone is velcome to do so.

**Would like to thank everyone working on all 3 repos of picom as without their work we (old timers) wouldn't have a good compositor so thank you guys for your hard work/time to keep it updated and maintenanced regularly!**

**IMPORTANT**: always run picom with `--experimental-backends` argument as otherwise animations won't really look good.

## Call for testers

### `--experimental-backends`

This flag enables the refactored/partially rewritten backends.

Currently, new backends feature better vsync with the xrender backend and improved input lag with the glx backend (for non-NVIDIA users). The performance should be on par with the old backends.

New backend features will only be implemented on the new backends from now on, and the old backends will eventually be phased out after the new backends stabilize.

To test the new backends, add the `--experimental-backends` flag to the command you use to run picom. This flag is not available from the configuration file.

To report issues with the new backends, please state explicitly you are using the new backends in your report.

## Change Log

See [Releases](https://github.com/yshui/picom/releases)

## Build

### Dependencies

Assuming you already have all the usual building tools installed (e.g. gcc, python, meson, ninja, etc.), you still need:

* libx11
* libx11-xcb
* libXext
* xproto
* xcb
* xcb-damage
* xcb-xfixes
* xcb-shape
* xcb-renderutil
* xcb-render
* xcb-randr
* xcb-composite
* xcb-image
* xcb-present
* xcb-xinerama
* xcb-glx
* pixman
* libdbus (optional, disable with the `-Ddbus=false` meson configure flag)
* libconfig (optional, disable with the `-Dconfig_file=false` meson configure flag)
* libGL (optional, disable with the `-Dopengl=false` meson configure flag)
* libpcre (optional, disable with the `-Dregex=false` meson configure flag)
* libev
* uthash

On Debian based distributions (e.g. Ubuntu), the needed packages are

```
libxext-dev libxcb1-dev libxcb-damage0-dev libxcb-xfixes0-dev libxcb-shape0-dev libxcb-render-util0-dev libxcb-render0-dev libxcb-randr0-dev libxcb-composite0-dev libxcb-image0-dev libxcb-present-dev libxcb-xinerama0-dev libxcb-glx0-dev libpixman-1-dev libdbus-1-dev libconfig-dev libgl1-mesa-dev libpcre2-dev libpcre3-dev libevdev-dev uthash-dev libev-dev libx11-xcb-dev meson
```

On Fedora, the needed packages are

```
dbus-devel gcc git libconfig-devel libdrm-devel libev-devel libX11-devel libX11-xcb libXext-devel libxcb-devel mesa-libGL-devel meson pcre-devel pixman-devel uthash-devel xcb-util-image-devel xcb-util-renderutil-devel xorg-x11-proto-devel
```

To build the documents, you need `asciidoc`

### To build

```bash
$ git submodule update --init --recursive
$ meson --buildtype=release . build
$ ninja -C build
```

Built binary can be found in `build/src`

If you have libraries and/or headers installed at non-default location (e.g. under `/usr/local/`), you might need to tell meson about them, since meson doesn't look for dependencies there by default.

You can do that by setting the `CPPFLAGS` and `LDFLAGS` environment variables when running `meson`. Like this:

```bash
$ LDFLAGS="-L/path/to/libraries" CPPFLAGS="-I/path/to/headers" meson --buildtype=release . build

```

As an example, on FreeBSD, you might have to run meson with:
```bash
$ LDFLAGS="-L/usr/local/lib" CPPFLAGS="-I/usr/local/include" meson --buildtype=release . build
$ ninja -C build
```

### To install

``` bash
$ ninja -C build install
```

Default install prefix is `/usr/local`, you can change it with `meson configure -Dprefix=<path> build`

## How to Contribute

### Code

You can look at the [Projects](https://github.com/yshui/picom/projects) page, and see if there is anything that interests you. Or you can take a look at the [Issues](https://github.com/yshui/picom/issues).

### Non-code

Even if you don't want to contribute code, you can still contribute by compiling and running this branch, and report any issue you can find.

Contributions to the documents and wiki will also be appreciated.

## Contributors

See [CONTRIBUTORS](CONTRIBUTORS)

The README for the [original Compton project](https://github.com/chjj/compton/) can be found [here](History.md#Compton).

## Licensing

picom is free software, made available under the [MIT](LICENSES/MIT) and [MPL-2.0](LICENSES/MPL-2.0) software
licenses. See the individual source files for details.
