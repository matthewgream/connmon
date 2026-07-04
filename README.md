# connmon

Connectivity monitor. Periodically checks that a host is reachable and its
inbound access is intact, and publishes the results as structured JSON to an
MQTT broker.

One of a small family of single-binary MQTT monitor daemons that share a common
structure and config convention:

- **hostmon** — this host's system + platform metrics
- **connmon** — outbound connectivity (UPnP port maps, reachability, dynamic DNS)
- **trafmon** — per-interface network traffic counters (via SNMP)

## What it does

On independent timers it runs up to three checks and emits an event for each:

- **`connectivity`** — HTTP GET a URL and report success + latency.
- **`upnp`** — ensure the configured inbound port-forward mappings exist on the
  local IGD/router (creates them if missing), so remote SSH/HTTP keeps working.
- **`cloudflare`** — dynamic DNS: keep a Cloudflare DNS record pointed at the
  host's current public IP.

Plus lifecycle events `startup` / `shutdown` and a periodic `heartbeat`. Any of
the three checks can be omitted from the config to disable it.

## Build

Single C source (`connmon.c`) with headers in `include/`; no runtime deps
beyond an MQTT broker (UPnP/HTTP/Cloudflare are done over the network).

    make                    # native build
    make armhf              # cross-compile for 32-bit ARM (armhf)
    make format             # clang-format the source
    make test               # build and run against the local .cfg
    make latency            # run the log-latency analyser (see below)
    make clean

## Install

**Host-specific config convention:** the Makefile installs
`connmon.<hostname>.cfg` if it exists, otherwise falls back to the generic
`connmon.cfg`. Commit one config per deployment host (e.g. `connmon.bastu.cfg`)
alongside the documented default `connmon.cfg`.

    make install-dev          # native:  binary -> /usr/local/bin/connmon
                              #          config -> /etc/default/connmon
                              #          unit   -> connmon.service (enabled)
    make install-dev-armhf    # same, from the armhf cross-build
    make remove-dev           # uninstall

Runs as the systemd service `connmon.service`; runtime config is
`/etc/default/connmon`.

## Configuration

Every setting is a config-file `key=value` **and** an equivalent `--key value`
command-line flag. `--config <file>` selects the file (default `connmon.cfg`).

### MQTT (common to hostmon / connmon / trafmon)

| key | default | meaning |
|---|---|---|
| `mqtt-server` | `mqtt://localhost` | broker URL |
| `mqtt-client` | `connmon` | client id |
| `mqtt-topic-prefix` | `system/connection` | base topic |
| `mqtt-tls-insecure` | `false` | skip TLS cert verification |
| `mqtt-reconnect-delay` | `5` | reconnect backoff start (s) |
| `mqtt-reconnect-delay-max` | `60` | reconnect backoff cap (s) |

### connmon-specific

| key | default | meaning |
|---|---|---|
| `heartbeat-period` | `60` | heartbeat interval (s) |
| `connectivity-check-period` | *(off)* | reachability check interval (s) |
| `connectivity-url` | *(none)* | URL to GET |
| `upnp-check-period` | *(off)* | UPnP mapping check interval (s) |
| `upnp-service[n]` | | mapping `n` description |
| `upnp-port-external[n]` | | external (WAN) port |
| `upnp-port-internal[n]` | | internal (LAN) port |
| `upnp-protocol[n]` | | `tcp` / `udp` |
| `cloudflare-check-period` | *(off)* | dynamic-DNS check interval (s) |
| `cloudflare-dns-name` | | DNS record to update |
| `cloudflare-zone-id` | | Cloudflare zone id |
| `cloudflare-token` | | Cloudflare API token |

A check is enabled only when its `*-check-period` (and required fields) are set;
`upnp-*[n]` entries are 1-indexed and repeatable.

## Output (MQTT)

Publishes each event to **`<mqtt-topic-prefix>/<hostname>`**. Every message is a
JSON object with `timestamp`, `hostname`, `event`, `success`, `message`, plus
event-specific fields.

```
$ mosquitto_sub -t 'system/connect/#'
{ "timestamp":1761738933, "hostname":"bastu", "event":"startup", "success":true, "message":"daemon started" }
{ "timestamp":1761738938, "hostname":"bastu", "event":"upnp", "success":true,
  "message":"1/1 succeeded - [1]: 'bastu-sshd-inbound' exists 192.168.0.226:22->…:9091 tcp" }
{ "timestamp":1761738938, "hostname":"bastu", "event":"connectivity", "success":true,
  "message":"connection to 'http://www.google.com' succeeded (HTTP 200, 151.497ms, 677 bytes)" }
{ "timestamp":1761738999, "hostname":"bastu", "event":"heartbeat", "success":true,
  "message":"daemon active (1)", "upnp_enabled":true, "upnp_mappings":1 }
```

## Companion tool: `analysis/latency.js`

Parses `connmon`'s journal and prints connectivity-latency percentile tables
(per-day and time-of-day histogram):

    journalctl -u connmon | analysis/latency.js
    # or:  make latency

## License

CC BY-NC-SA 4.0 — see [LICENSE](LICENSE).
