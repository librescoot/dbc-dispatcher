# Librescoot DBC Dispatcher

Part of the [Librescoot](https://librescoot.org/) open-source platform.

## Overview

`dbc-dispatcher` is the systemd-based display application supervisor for a
Librescoot Dashboard Computer (DBC). It selects a display application from the
vehicle datastore, starts and stops its systemd unit, and remains running to
apply later changes.

## Capabilities

- Starts the configured display application through systemd's private D-Bus
  interface.
- Starts the last successfully selected application before the datastore is
  reachable, improving display startup after boot.
- Reconciles the selected application when datastore connectivity returns.
- Watches for live application changes and rolls back to the previous unit if
  the replacement does not start.
- Applies `settings[scooter.logserver]`: rewrites
  `/etc/systemd/journal-upload.conf` and enables/restarts
  `systemd-journal-upload` when set, stops and disables it when unset — the
  same settings-service behaviour the MDB applies to its own copy.
- Responds to a dashboard power-off command and handles orderly termination.
- Temporarily displays a local image or looping video, or downloads one over HTTP(S) before switching the display. Stopping playback restores the configured application.

## Operation and interfaces

The dispatcher reads `settings[dashboard.app]`; its value is mapped to a
systemd unit by appending `.service` when needed. If the setting is absent, the
default is `scootui-qt`. It subscribes to these pub/sub channels:

| Channel | Payload | Action |
| --- | --- | --- |
| `settings` | `dashboard.app` | Re-read the setting and switch the managed unit |
| `settings` | `scooter.logserver` | Re-read the setting and (re)apply journal-upload |
| `dbc:command` | `poweroff` | Invoke `poweroff` |
| `dbc:command` | `media:play <source>` | Cache and display an MP4, JPEG or PNG from an absolute DBC path or HTTP(S) URL |
| `dbc:command` | `media:stop` | Stop media playback and restore the configured dashboard |
| `dbc:command` | `media:clear` | Delete cached items except the one currently playing; cancel an in-progress fetch |

The active selection is persisted in `/var/lib/dbc-dispatcher/last-app` after a
successful switch. At startup, that cache is used before a datastore connection
is available; the dispatcher then reconciles it with `settings[dashboard.app]`.

`dbc-dispatcher --version` prints the compiled version. All other operation is
through its systemd unit, for example:

```sh
systemctl status dbc-dispatcher.service
journalctl -u dbc-dispatcher.service
```

## Configuration

There is no configuration file or command-line configuration. The datastore
host and port are compiled as `192.168.7.1:6379`, and the default application is
`scootui-qt`. Configure the selected application by writing the settings hash
and publishing the changed key:

```sh
redis-cli HSET settings dashboard.app scootui-qt
redis-cli PUBLISH settings dashboard.app
```

The configured value names a systemd unit. Only select units that are installed
and appropriate for the DBC.

## Temporary media display

The command source must end in `.mp4`, `.jpg`, `.jpeg`, or `.png` (a URL query string is allowed). The DBC must have `curl` and `ffmpeg`, and its framebuffer must be `/dev/fb0` with a 480×480 display accepting `bgra` pixels. For example, with a file on the MDB's data-server:

```sh
redis-cli PUBLISH dbc:command 'media:play http://192.168.7.1:8080/mockup.mp4'
redis-cli PUBLISH dbc:command 'media:stop'
redis-cli PUBLISH dbc:command 'media:clear'
```

For a file already on the DBC, use `media:play /data/mockup.png`. Downloads and local copies are cached under `/data/dbc-dispatcher/` (100 MiB per item, 120-second HTTP timeout). The current display stays up until the copy succeeds; failure leaves it unchanged. MP4 playback loops and images remain on screen. A new play request switches after its copy succeeds. `dashboard.app` changes during playback take effect on stop. Playback is transient: reboot starts the configured dashboard, not the media.

The cache is keyed by source path or URL. Replaying a cached URL uses its local copy without re-fetching; add a distinct query string or clear the cache to refresh it. A local file is re-copied when its modification time is newer than the cached copy. The cache retains up to **8 items / 256 MiB**, evicting the least recently played items after each successful play. The most recent item is protected during automatic eviction, including while playing. `media:stop` keeps it for reuse across restarts. `media:clear` removes everything except an item currently playing; after stopping playback, it removes that item too. An in-progress fetch is canceled by either stop or clear.

`journalctl -u dbc-dispatcher` reports fetch and playback failures. `media:stop` and `media:clear` are safe to send when idle.

## Build and test

The project has a single C source file. Build and test with:

```sh
make build-host   # host binary in bin/dbc-dispatcher
make build        # ARMv7 cross-build in bin/dbc-dispatcher
make dist         # ARMv7 build, then strip it
make test         # Host media validation tests
```

Host builds require `gcc`, `pkg-config`, and development packages for
`libsystemd` and `hiredis`. The ARM build additionally requires
`arm-linux-gnueabihf-gcc`, `arm-linux-gnueabihf-strip`, and ARM-target
`pkg-config` metadata for those libraries.

## Deployment and runtime dependencies

The Yocto recipe installs `/usr/bin/dbc-dispatcher` and enables
`dbc-dispatcher.service`. At runtime it requires systemd (including
`/run/systemd/private`), a Redis-compatible server at the compiled endpoint,
and the display application units it may manage. Temporary media also requires
`curl`, `ffmpeg`, `/data`, and the DBC framebuffer. The service runs as root,
restarts after three seconds on failure, and logs to the system journal.

## Operational notes

The dispatcher is privileged: a datastore setting determines which systemd unit
it starts, and the `poweroff` command powers off the host. Restrict write and
publish access to the datastore accordingly. A failed replacement application
is reverted, but an invalid cached application can still delay startup until the
fallback or datastore reconciliation succeeds.

## License

This project is licensed under the [Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License](LICENSE).

Made with ❤️ by the Librescoot community
