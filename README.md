# dbc-dispatcher

Manages display applications on the Dashboard Computer (DBC) as systemd units. Reads the configured app from Redis, starts it via D-Bus, and watches for live app switches and power commands.

Part of the [Librescoot](https://librescoot.org/) open-source platform.

Written in C for fast startup on the i.MX6 DL (ARMv7). Statically linked, no runtime dependencies.

## How it works

1. Connect to Redis and systemd D-Bus
2. Read `HGET settings dashboard.app` for the app name
3. Start `<app>.service` via D-Bus (falls back to `scootui-qt` on failure)
4. Subscribe to Redis PUBSUB on `settings` and `dbc:command`
5. On app change: stop current unit, start new one (reverts on failure)
6. On `poweroff` command: stop current unit, run `poweroff`

The dispatcher stays running for the lifetime of the DBC session.

## Build

Requires `libsystemd-dev` and `libhiredis-dev`. For cross-compilation, install the `armhf` variants plus `gcc-arm-linux-gnueabihf`.

```sh
# ARM target (production, armv7 static binary)
make build

# Host platform (development, dynamically linked)
make build-host

# Stripped ARM binary for distribution
make dist
```

## Flags

| Flag | Description |
|---|---|
| `--version` | Print version and exit |

Redis host (`192.168.7.1:6379`) and timeout (5s) are compiled in. If Redis is unreachable after the timeout, the dispatcher continues with the default app.

## Redis API

| Operation | Key/Channel | Field | Description |
|---|---|---|---|
| `HGET` | `settings` | `dashboard.app` | App name (read at startup) |
| `SUBSCRIBE` | `settings` | -- | Watches for setting changes (filters for `dashboard.app` payload) |
| `SUBSCRIBE` | `dbc:command` | -- | Commands (`poweroff`) |

The app name maps directly to a systemd unit: `scootui-qt` -> `scootui-qt.service`. Default is `scootui-qt`.

## App switching

Change the app via the settings hash:

```sh
redis-cli hset settings dashboard.app carplay
redis-cli publish settings dashboard.app
# dispatcher re-reads the value, stops scootui-qt.service, starts carplay.service
```

If the new unit fails to start, the dispatcher reverts to the previous one.

## License

This project is dual-licensed. The source code is available under the
[Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License][cc-by-nc-sa].
The maintainers reserve the right to grant separate licenses for commercial distribution; please contact the maintainers to discuss commercial licensing.

[![CC BY-NC-SA 4.0][cc-by-nc-sa-image]][cc-by-nc-sa]

[cc-by-nc-sa]: http://creativecommons.org/licenses/by-nc-sa/4.0/
[cc-by-nc-sa-image]: https://licensebuttons.net/l/by-nc-sa/4.0/88x31.png
