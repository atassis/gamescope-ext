# External upscaler ABI

`-F external:<name|path.so>[:config]` hands the upscale pass that FSR1/NIS would otherwise run to a
plugin: on an NPU, a second GPU, or the CPU. ABI: `src/external_upscaler.h` (plain C, one exported
symbol `gamescope_external_upscaler_get_api`, a struct of function pointers, `abi_version` plus a
`size` on every struct so either side can grow by appending fields). Host loader:
`src/external_upscaler_host.{hpp,cpp}`. Consumer: `vulkan_external_upscale_layer0()` in
`src/rendervulkan.cpp`.

## Discovery

A name resolves to `<dir>/<name>/manifest.json` over `$GAMESCOPE_UPSCALER_PATH`, then
`$XDG_DATA_HOME/gamescope-upscalers`, the host's own `<prefix>/share/gamescope-upscalers`, then
`$XDG_DATA_DIRS` (default `/usr/local/share:/usr/share`) + `/gamescope-upscalers`. The first hit
wins; a manifest whose `abi_version` differs from the host's is refused. A spec containing `/` is a
library path. Keys the host reads: `name`, `abi_version`, `library` (relative to the manifest),
`default_config` (used when the `-F` value carries none), `description`; `device` is for people.
Three optional keys are for people and for `gamescope-ext-run --list`, and the host ignores them:
`config_help` (the config string's syntax), `configs` (a list of `{name, description, scales}`, one
per data directory installed beside the plugin, scales written like `3/2`) and `env` (an object of
environment variable to what it does).

The host installs `external_upscaler.h` and a `gamescope-external-upscaler` pkg-config file; each
plugin is its own meson project and builds against either.

## Shape

1. **Device identity**: `get_device_info()` reports kind (CPU/GPU/NPU) and a DRM render-node `dev_t`
   when the device has one.
2. **Negotiation**: `negotiate(in/out size, colorspace, hdr)` accepts or declines, and lists up to 8
   input formats, most preferred first: plane kind, DRM fourcc, modifier, edge padding, alignment
   and the full padded plane sizes. The host serves unpadded BGRA8, or a padded Y8 plane in with
   NV12 out, and only at `DRM_FORMAT_MOD_LINEAR`. A plugin that cannot reach the shown size
   exactly may accept a smaller `scale_num/scale_den`; the host stretches its output the rest of
   the way. HDR layers are offered like any other, with `hdr` set.
3. **Async submit**: `submit(in dmabuf, out dmabuf, in_fence_fd) -> out_fence_fd`. The readback
   reaches the plugin as a sync_file `in_fence_fd`. `-1` means the output is already written;
   `-2` is a hard failure.
4. **Lifecycle**: `create`/`destroy`, and `negotiate()` again at any time, e.g. on a layer resize.

A selected plugin is never replaced by another upscaler. When it cannot be found, gamescope does not
start; when it declines, fails `submit()`, or the host cannot serve it (a YCbCr layer, no plane it
can produce, allocation failure), gamescope logs the plugin, the sizes and the reason, and exits
with status 1.

The host submits at commit time into one of four input/output slot pairs, at most two in flight,
and composites the newest finished result.

## Plugins

A plugin is its own meson project. Its installer checks for the plugin's hardware, builds against
the installed header, and puts the library and `manifest.json` in `<dir>/<name>/` on the search
path above.

## Known gaps

- Cross-device import (a plugin on a second GPU) has not been run: the test machine has one GPU.
- `writes_into_host_buffer=false` is not implemented on the host side.
- Only LINEAR is ever offered; a plugin that needs a tiled modifier has no path.
- The host only `poll()`s the returned fence, so a plugin may return any pollable fd (an eventfd,
  say) instead of a sync_file; a host that imported it into Vulkan would reject it. A device with
  no DRM render node (`/dev/accel/accelN`) reports `dev_node` 0.
- Re-negotiation on a live resize has not been exercised.
