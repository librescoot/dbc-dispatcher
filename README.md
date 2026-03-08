# dbc-dispatcher

Manages display applications on the Dashboard Computer (DBC) as systemd units. Reads the configured app from Redis, starts it via D-Bus, and watches for live app switches and power commands.

## How it works

1. Connect to Redis and systemd D-Bus
2. Read `HGET settings dashboard.app` for the app name
3. Start `<app>.service` via D-Bus (falls back to `scootui-qt` on failure)
4. Subscribe to Redis PUBSUB on `dashboard.app` and `dbc:command`
5. On app change: stop current unit, start new one (reverts on failure)
6. On `poweroff` command: stop current unit, run `poweroff`

The dispatcher stays running for the lifetime of the DBC session — it doesn't exec into the target app.

## Build

```sh
# ARM target (production, armv7)
make build

# Host platform (development)
make build-host
```

## Flags

| Flag | Default | Description |
|---|---|---|
| `--redis-url` | `redis://192.168.7.1:6379` | Redis server URL |
| `--timeout` | `5s` | Redis connection timeout |
| `--version` | — | Print version and exit |

If Redis is unreachable after the timeout, the dispatcher continues anyway and uses the default app.

## Redis API

| Operation | Key/Channel | Field | Description |
|---|---|---|---|
| `HGET` | `settings` | `dashboard.app` | App name (read at startup) |
| `SUBSCRIBE` | `dashboard.app` | — | App switch notifications |
| `SUBSCRIBE` | `dbc:command` | — | Commands (`poweroff`) |

The app name maps directly to a systemd unit: `scootui-qt` -> `scootui-qt.service`. Default is `scootui-qt`.

## App switching

Publishing to the `dashboard.app` channel triggers a live switch:

```sh
redis-cli publish dashboard.app carplay
# stops scootui-qt.service, starts carplay.service
```

If the new unit fails to start, the dispatcher reverts to the previous one.
