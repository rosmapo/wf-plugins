# wayfire-rounded-corners

Wayfire plugin that rounds off window corners as a post-process on
each window's own texture (antialiased, soft edge) — the same basic
idea as Hyprland's `decoration:rounding`. The radius is a normal
Wayfire config option, so it can be changed live from WCM.

Targets **Wayfire 0.9.0** as packaged in Debian 13 "trixie".

## Files

- `rounded-corners.cpp` — the plugin itself (a `view_transformer_t`
  attached to every mapped toplevel window, plus the plugin glue that
  attaches/detaches it and reacts to config changes).
- `rounded-corners.xml` — metadata describing the plugin and its
  `radius` option. This single file is what:
  - tells Wayfire's config backend the option's name/type/default/range,
  - makes the plugin and the "Corner radius" slider show up
    automatically in **WCM** (Wayfire Config Manager) — WCM has no
    separate config format, it just reads plugin metadata XML files
    and builds the UI from them.
- `meson.build` — build script.

## Build dependencies (Debian 13)

```sh
sudo apt install build-essential meson ninja-build pkg-config \
    wayfire-dev libwf-config-dev libwlroots-dev libglm-dev libpixman-1-dev
```

## Build & install

```sh
meson setup build --buildtype=release
ninja -C build
sudo ninja -C build install
```

This installs:

- the plugin `.so` into `<prefix>/lib/wayfire/rounded-corners.so`
- the metadata into wayfire's metadata dir (normally
  `/usr/share/wayfire/metadata/rounded-corners.xml`)

## Enable it

Add `rounded-corners` to the plugin list in `~/.config/wayfire.ini`:

```ini
[core]
plugins = ... rounded-corners
```

or just tick the "Rounded Corners" checkbox in WCM once the metadata
is installed — WCM will pick up the plugin and its radius slider
automatically, no extra WCM-specific file is needed.

Restart Wayfire (or reload config) for the plugin list change to take
effect.

## Notes / things worth knowing

- The radius is read live in the shader's uniform upload, and the
  plugin forces a repaint of all views when the option changes, so
  moving the WCM slider updates the corners immediately without
  needing to remap windows.
- Only views with `role == VIEW_ROLE_TOPLEVEL` are rounded (panels,
  backgrounds, docks etc. are left alone).
- Corner rounding is implemented with a `discard`-free smoothstep mask
  (1px antialiased band) rather than a hard `discard`, which is closer
  to how Hyprland's rounding looks than the harder-edged version from
  the original 2020 "Writing Wayfire Plugins" tutorial this code is
  based on.

### If it doesn't compile as-is

Wayfire's C++ plugin API has shifted a few times around the
scene-graph rewrite (transformer attachment moved from
`view->add_transformer(unique_ptr<...>)` to
`view->get_transformed_node()->add_transformer(shared_ptr<...>, z_order, name)`,
and signal handling moved from `wf::signal_connection_t` /
`wf::get_signaled_view()` to typed `wf::signal::connection_t<T>`).
This code targets the API shape used by Wayfire 0.9–0.11 era plugins
(matching `plugins/single_plugins/wrot.cpp` and
`plugins/animate/basic_animations.hpp` in the upstream tree, and the
`timgott/wayfire-shadows` plugin, which is packaged for Debian 13
against the same `wayfire-dev (>= 0.9~)`). If your exact point release
differs slightly, the two spots most likely to need a tweak are:

1. The `add_transformer` call in `on_view_mapped` — check the exact
   signature in `/usr/include/wayfire/scene.hpp` and
   `/usr/include/wayfire/view-transform.hpp` on your system
   (`dpkg -L wayfire-dev | grep view-transform`).
2. The signal connection style — check `/usr/include/wayfire/signal-provider.hpp`.
