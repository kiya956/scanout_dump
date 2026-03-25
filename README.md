# scanout_dump

A tool to dump the DRM scanout framebuffer to a PNG image. It reads the raw pixel data directly from a DRM CRTC framebuffer via the kernel DRM API, then converts it to PNG using `ffmpeg`.

## Dependencies

### libdrm

```bash
sudo apt install libdrm-dev
```

### ffmpeg

```bash
sudo apt install ffmpeg
```

## Build

```bash
make
```

## Usage

```bash
sudo ./dump_scanout.sh <card_name> <crtc_id>
```

- `<card_name>`: DRM card device name under `/dev/dri/` (e.g. `card0`)
- `<crtc_id>`: CRTC ID to dump (find with `modetest` or `drm_info`)

**Example:**

```bash
sudo ./dump_scanout.sh card0 52
```

This produces `scanout.png` in the current directory.

> **Note:** The tool requires DRM master access. Run as root or with appropriate permissions if you get `handle=0` errors.

## How it works

1. `dump_fb` opens the DRM device, retrieves the framebuffer attached to the given CRTC, maps it via `DRM_IOCTL_MODE_MAP_DUMB`, and writes the raw BGRA pixel data to `/tmp/scanout.raw`.
2. `dump_scanout.sh` calls `dump_fb`, then invokes `ffmpeg` to convert the raw file to `/tmp/scanout.png`, and moves it to the current directory.
