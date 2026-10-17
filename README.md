# kaochatproxy

A small C chat proxy for Anarchy Online and Nadybot. The executable is named
**kworker**. It runs up to 256 worker characters, distributes buddy tracking,
and sends tells through independent per-worker queues. No SQL database is needed.

The buddy, tell, and Nadybot proxy services are separate shared-library plugins.
The proxy handles its sockets on a dedicated thread. AO connections are polled
together by one I/O owner, keeping libaochat calls serialized; queues between
the services are protected by mutexes.

## Dependencies and build

Linux, a C compiler, GNU Make, OpenSSL development headers/libraries, and
Python 3 for the tests are required. On Debian or Ubuntu:

```sh
sudo apt install build-essential libssl-dev python3 git
git clone https://github.com/fishman/libaochat.git
git clone https://github.com/axlslak/kaochatproxy.git
cd kaochatproxy
make
make check
```

Get **[libaochat from fishman/libaochat](https://github.com/fishman/libaochat)**.
Keep the two checkouts next to each other, or use
`make LIBAOCHAT_DIR=/path/to/libaochat`. The build stages dependency sources in
`.build` and leaves that checkout unchanged. There is no need to install
libaochat system-wide.

Build outputs are `kworker` and `kworker-plugins/{buddy,tell,nady_proxy}.so`.

## Configure workers

```sh
cp kworker.conf.example kworker.conf
chmod 600 kworker.conf
# Edit kworker.conf with your worker credentials.
./kworker
```

Repeat the following section for each worker character (maximum 256):

```ini
[worker]
username=your-account
password=your-password
character=Yourworker
```

Characters must be distinct. Multiple characters may use the same account.
Each connection contributes up to 1,000 buddy slots; the leader's connection
also contributes 1,000. Existing buddies consume slots.

Configure Nadybot to use this proxy's reachable LAN address and port 9993.
Nadybot retains its own login credentials and leader connection. This process
accepts one Nadybot client at a time. Worker accounts are additional chat
connections, not replacements for the leader.

For distributing paged tells in Nadybot, enable `force_mass_tells`; disable
`paging_on_same_worker` if you want pages spread among workers. Pages sent by
different workers can arrive out of order. Each worker's tell queue is paced
independently at one tell per second.

## Worker service for City Dwellers

To let a service use the already logged-in workers without its own AO login,
start the updated host and plugins in worker mode:

```sh
KWORKER_PROXY_MODE=workers ./kworker
```

The listener uses the same LAN address and port 9993. All AO usernames,
passwords and character names stay in `kworker.conf`. The service connection
does not create or authenticate a leader. Names are resolved through online
workers, and buddy assignments use only worker capacity. The default `leader`
mode retains the existing Nadybot login relay.

City Dwellers configuration, inside the existing `Manager` object:

```json
"ChatProxy": {
    "Enabled": true,
    "Host": "192.168.23.38",
    "Port": 9993
}
```

Manager is the sole client; other City Dwellers components use Manager memory.
Existing City Dwellers tell delivery is unaffected. Rebuild the kworker host
and all plugins together for the added worker lookup API; reloading just the
plugin into an older host is insufficient. This mode also serves one client
at a time, so a separate Nadybot client cannot share this listener concurrently.

Worker mode uses the existing big-endian AO chat frame envelope. On connect it
sends packet 100 containing an AO string of capabilities JSON, including
`"connection-mode":"workers"`. Packet 100 `capabilities` requests return the
same description; other valid ping strings are echoed. Client packet 21 is an
AO string name; server packet 21 is UID32 plus an AO string name, obtained from
the workers' AO replies. Buddy add/remove/status and worker tags retain their
existing packet formats. Unsupported packets close the service connection;
no fallback leader login is attempted. A disconnect never means a tracked
player went offline; clients must resubscribe and await fresh buddy reports.

## Environment settings

| Variable | Default / purpose |
| --- | --- |
| `KWORKER_CONFIG` | `kworker.conf` |
| `AOUSER`, `AOPASS`, `AOCHAR` | Legacy single-worker configuration when no config file is used |
| `AOSERVER`, `AOPORT` | Worker chat server: `chat.d1.funcom.com`, `7105` |
| `NADY_LISTEN_HOST` | An active `192.168.0.0/16` interface; otherwise `127.0.0.1` |
| `NADY_PORT` | Proxy listener port: `9993` |
| `KWORKER_PROXY_MODE` | `leader` for Nadybot; `workers` for a service using existing worker logins |
| `NADY_UPSTREAM_HOST`, `NADY_UPSTREAM_PORT` | Leader chat server: `chat.d1.funcom.com`, `7105` |
| `KWORKER_PLUGIN_DIR` | `./kworker-plugins` |

Set `NADY_LISTEN_HOST` explicitly when the machine has multiple LAN interfaces.
Use the listener only on a trusted network and restrict access with a firewall:
leader mode forwards the client's AO login exchange. Neither mode adds TLS or
a separate proxy authentication layer.

## Behavior and limits

- Buddy assignments balance across the leader and connected workers, with
  dynamically growing routing state up to the configured connection capacity.
- Worker buddy operations are queued independently and paced at four per
  second per worker. Large imports can take time to drain.
- Supported tell modes: round-robin, by-charid, by-msgid, proxy-default,
  and by-worker.
- There is no automatic worker reconnect. A failed worker connection stops the
  process; restart it and reconnect Nadybot.
- Restart to apply account configuration changes.
- `SIGHUP` reloads plugins while retaining worker AO connections, but closes
  the Nadybot connection and discards plugin queues and routing snapshots.
  Reconnect Nadybot afterward; use a full restart for routine upgrades.

Run `make check` for configuration, routing, parallel tell scheduling,
large buddy queue, and source policy checks. Tests do not log in to AO.

## Credits

**[Nadybot's aochatproxy](https://github.com/Nadybot/aochatproxy)** established
the proxy protocol and behavior that this project implements for Nadybot
compatibility. Thank you to Nady and the aochatproxy contributors.

**[libaochat](https://github.com/fishman/libaochat)** supplies the AO chat
connection library. Its upstream credits include Andreas Allerdahl and
Oskari Saarenmaa (auno.org). It is licensed under GPL version 2 or later,
not LGPL; consult its source notices when using or distributing linked builds.
