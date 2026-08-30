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
- Responds to a dashboard power-off command and handles orderly termination.

## Operation and interfaces

The dispatcher reads `settings[dashboard.app]`; its value is mapped to a
systemd unit by appending `.service` when needed. If the setting is absent, the
default is `scootui-qt`. It subscribes to these pub/sub channels:

| Channel | Payload | Action |
| --- | --- | --- |
| `settings` | `dashboard.app` | Re-read the setting and switch the managed unit |
| `dbc:command` | `poweroff` | Invoke `poweroff` |

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

## Build and test

The project has a single C source file and no automated test target. Build with:

```sh
make build-host   # host binary in bin/dbc-dispatcher
make build        # ARMv7 cross-build in bin/dbc-dispatcher
make dist         # ARMv7 build, then strip it
```

Host builds require `gcc`, `pkg-config`, and development packages for
`libsystemd` and `hiredis`. The ARM build additionally requires
`arm-linux-gnueabihf-gcc`, `arm-linux-gnueabihf-strip`, and ARM-target
`pkg-config` metadata for those libraries.

## Deployment and runtime dependencies

The Yocto recipe installs `/usr/bin/dbc-dispatcher` and enables
`dbc-dispatcher.service`. At runtime it requires systemd (including
`/run/systemd/private`), a Redis-compatible server at the compiled endpoint,
and the display application units it may manage. The service runs as root,
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
