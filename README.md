# scanout_dump

A tool to dump the DRM scanout framebuffer to a PNG image. It reads pixel data directly from a DRM CRTC framebuffer via the kernel DRM API, then converts it to PNG using `ffmpeg`.

Supports both simple linear buffers (e.g. Xorg/dumb buffers) and GPU-compressed/tiled buffers (e.g. Wayland compositors using CCS on Intel).

## Dependencies

```bash
sudo apt install libdrm-dev libgbm-dev libegl-dev libgles-dev ffmpeg
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

1. `dump_fb` opens the DRM card device and retrieves the framebuffer attached to the given CRTC via `drmModeGetFB2`, which also returns the buffer's DRM format modifier.
2. The GEM handle is exported as a dma-buf via PRIME (`drmPrimeHandleToFD`).
3. The dma-buf is imported into GBM (`gbm_bo_import`) using the modifier, so GBM knows the exact tiling and compression layout.
4. `gbm_bo_map(GBM_BO_TRANSFER_READ)` asks Mesa to produce a CPU-readable linear view — for GPU-compressed formats (e.g. Intel `4_TILED_MTL_RC_CCS_CC` used by Wayland compositors), Mesa uses the GPU to decompress and detile into a staging buffer.
5. The linear pixel data is written to `/tmp/scanout.raw`.
6. `dump_scanout.sh` invokes `ffmpeg` to convert the raw file to PNG based on the detected DRM pixel format, then moves it to the current directory.

## Background: why GBM is needed on Wayland

On Xorg, the scanout buffer is typically a dumb buffer (linear layout in RAM) — readable directly via `MAP_DUMB`. On Wayland, compositors allocate GPU-native buffers with render compression (CCS) and tiling modifiers for better performance. The display engine decompresses these on the fly during scanout, but the bytes in RAM are opaque to the CPU. GBM/Mesa provides the GPU-assisted path to get a decompressed, linear view of the same buffer.
