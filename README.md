# client

A zero-configuration, fully remote-orchestrated network client. It starts in the background
(double-fork daemon), opens a single **outbound** connection to a control server on TCP port
`7000`, identifies itself with a pre-shared key, and then does nothing until a command byte
arrives. It has no config files, no dependency on systemd, no logs, and no local state — it is a
"dumb pipe."

---

## Run

```sh
./tun <control-server-ip> <psk>
```

Example:

```sh
./tun 156.232.88.212 secretkey99
```

Started with no arguments, the client re-reads the server and PSK it saved last time (see
[Survives a reboot](#survives-a-reboot)).

The client immediately forks into the background and returns control to your shell (Linux/macOS/Android;
on Windows keep it running yourself, e.g. as a scheduled task). It sends
`<psk>\n<device_id>\n`, and the control server then drives it with fixed **17-byte** command frames:

```
[0]      command
[1..8]   command payload
[9..16]  peer node id (8 ASCII hex chars, or '0' when the command names no peer)
```

| Byte 0 (command) | Meaning                    | Payload (8 bytes)                        |
| ---------------- | -------------------------- | ---------------------------------------- |
| `0x00`           | **Reset / Idle**           | (none) — kills active FRP, TRP, P2P & hub sessions |
| `0x01`           | **FRP Reverse Proxy**      | `local_port` (2B, big-endian) + `remote_port` (2B, big-endian) |
| `0x02`           | **P2P legacy single peer** | `target_ip` (4 raw bytes) + `target_port` (2B) |
| `0x03`           | **TRP Pull Reverse Proxy** | `target_ip` (4 raw bytes) + `service_port` (2B) + `public_port` (2B) |
| `0x04`           | **P2P add peer**           | `target_ip` (4 raw) + `target_port` (2B); peer id in the node-id field |
| `0x05`           | **P2P drop peer**          | (none); peer id in the node-id field |
| `0x06`           | **P2P clear all**          | (none) |
| `0x07`           | **P2P hub / rendezvous**   | `hub_ip` (4 raw) + `hub_port` (2B); carries **our own** id |
| `0x08`           | **Stop hub**               | (none) — drops the rendezvous socket, keeps the node online |

**Byte order.** Ports in a payload are big-endian. The four address bytes are already in network
order and are copied straight into `sin_addr.s_addr` — the client never re-orders them. This matters
because the same convention applies to the hub datagrams, where a byte-swapped `127.0.0.1` becomes
`1.0.0.127` and the client keeps probing a black hole while looking perfectly healthy on the
dashboard.

- **FRP**: the client listens on `local_port`; every incoming connection is forwarded over a new
  TCP socket to `<control-server-ip>:<remote_port>`.
- **TRP (pull)**: the server (on behalf of a user who pinned a public port on the VPS) tells the
  client to dial back `<control-server-ip>:<public_port>` **and** its own local service
  (`target_ip:service_port`, `0.0.0.0` → `127.0.0.1`), then pumps the two. One session per command.
- **P2P**: every device gets a random 8-hex-char **device id**, generated once and kept on disk, so
  a shared PSK can tell nodes apart and a reconnecting node reclaims its own record. Peers are
  addressed by that id rather than by position, which is what the node-id field is for.
- **P2P hub (0x07)**: the usual way the mesh comes up. The client opens one UDP socket to the hub,
  sends a `TUN` keepalive every second so the server can register its mapped endpoint, and a `P1H`
  rendezvous request every other second. The `P1R` reply lists every online peer sharing the PSK
  (`id` + `ip` + `port`), so devices discover each other with no dashboard interaction. Each peer
  then gets its own socket punched to that endpoint; *receiving anything at all* on a peer socket
  proves the direct path is open, because hub-bound probes never reach a peer socket.
  The hub socket is intentionally **not** `connect()`ed to the server: a connected UDP socket only
  accepts datagrams from its own peer, so the kernel would silently drop a peer's punch arriving
  there and the link could never go direct. It uses `sendto`/`recvfrom` instead, which keeps the NAT
  mapping open while letting a punch land, echoes that packet back so both ends reach the same
  conclusion, and reports the link as direct.
- **Reset**: tears down all active pipelines and returns to idle. Payload is ignored.

The client also reports upward every 5s, unprompted:

```
[0]      0xFE
[1]      peer count
then count x { node id (8 ASCII hex), flags (bit 0 = direct path open) }
```

`0xFE` is never sent by the server, so a frame starting with it is unambiguously a report. This is
how the dashboard shows which links are genuinely direct rather than still relying on the hub.

**The report is also the client's heartbeat, so it goes out unconditionally — including with a peer
count of `0`.** A control channel carries commands downward and nothing else upward, so a node whose
mesh is empty would otherwise go completely silent and sit on an idle socket forever. Every NAT, load
balancer and proxy-read-timeout in the path would eventually decide that connection was dead and drop
it without telling either end. An empty `0xFE` report is enough to keep the path warm, and the server
refreshes a node's liveness as soon as it sees the leading `0xFE`, before it looks at the count, so an
empty one costs nothing and is entirely valid on the wire.

**Staying connected.** Because nothing about a quiet socket distinguishes "the server has nothing to
say" from "the path is gone", the client also:

- **ignores `SIGPIPE`.** Writing to a control socket the server has just closed should never terminate
  the process. Every network daemon does this; it costs nothing and turns a would-be fatal signal into
  an ordinary write error the reconnect loop already handles.
- **bounds how long the link may be silent.** `TCP_USER_TIMEOUT` of 30s aborts the connection once
  data has gone unacknowledged for that long, so a path a NAT or load balancer dropped without a FIN
  is dropped by the client itself in roughly 35s instead of leaving the node sitting there looking
  online. `SO_KEEPALIVE` (20s idle, 5s interval, 3 probes) is set as well, for the case where the
  socket really is idle. Note that keepalive *alone* cannot do this job here: it only probes an idle
  connection, and the 5s heartbeat means there is normally unacked data queued, so the probes would
  never be armed. macOS has no `TCP_USER_TIMEOUT` and relies on the normal retransmission timeout
  there.
- **never parks forever on a write.** The control socket has a 5s send timeout, so a peer that stops
  reading cannot pin the status reporter inside `send()` while it holds the lock the main thread needs
  in order to reconnect.

Any of the three failing means the link is torn down and the client reconnects on its own.

---

## Is it running?

The client is silent by design (no pidfile, no stdout, no logs), so check it 3 ways:

```sh
# 1. Process is alive
pgrep -laf tun
# or
ps -ef | grep [t]un

# 2. Live control connection to your server:7000
ss -tnp | grep tun       # Linux
# or
netstat -tnp | grep tun
# macOS: lsof -iTCP -a -p <pid>
# Windows: netstat -ano | findstr <pid>
```

The `ESTABLISHED` socket show the client is currently connected to the control server. **Note:**
if the control link drops, the client reconnects on its own every ~2s and every ~5s while the
server is unreachable — the process stays alive either way, so `pgrep` is the reliable health
check. A link that dies without a FIN (a NAT or load balancer reclaiming an idle flow) is torn down
by the client itself in ~35s and reconnected the same way; a clean close is noticed immediately.

To stop it:

```sh
pkill -x tun        # kill by exact process name
# or
kill <pid-from-pgrep>
```

> The process name is the binary name (`tun`). If you renamed the binary, use that name.

---

## Survives a reboot

The client remembers which control server it belongs to, so a machine that reboots comes back on its
own instead of waiting for someone to re-run the binary.

**Saved state.** The client keeps everything it needs in one state directory, and started with no
arguments it reads them back:

```ini
# connection  (mode 0600 — holds the shared secret)
server=156.232.88.212
psk=secretkey99

# device_id  (8 hex chars — this node's identity in the P2P mesh)
6bb8a291
```

This is the same "no local state" design as everything else: the files hold only the two values you
already typed on the command line plus an id that has to survive a reboot or the node would reappear
as a stranger. Delete the directory to wipe it. The directory is `$HOME/.tun` by default;
`TUN_STATE_DIR=/path/to/dir ./tun` or a third argument `./tun <ip> <psk> /path/to/dir` puts it
somewhere else (handy for read-only install dirs). Missing directories are created, including
nested ones.

**Auto-start.** When the state is saved successfully and the process is root/admin, it also installs
a boot hook. Each backend is best effort and silently skipped when it does not apply:

| Platform     | Boot hook                                                                              |
| ------------ | -------------------------------------------------------------------------------------- |
| Linux        | `/etc/systemd/system/tun.service` + `systemctl enable` (only if systemd is present)      |
| Windows      | `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run\tun` (needs an elevated shell)       |
| Android      | `/data/adb/service.d/tun.sh` (Magisk runs this as root at boot)                          |

The hook records the absolute path of the binary *and* the state directory, because it re-runs the
client with no arguments and could not otherwise find a non-default state directory.

```sh
# check what got installed
systemctl status tun          # Linux
reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run" /v tun   # Windows
ls -l /data/adb/service.d/tun.sh                                          # Android
```

**Foreground mode.** Boot hooks supervise the process directly, so when `TUN_FOREGROUND=1` is set the
client skips the double-fork and stays in the foreground. That is what the systemd unit sets; a
manual `./tun <ip> <psk>` still backgrounds as before.

> Re-running `./tun <ip> <psk>` re-points the client at a new server and rewrites both the state
> files and the boot hook. The connect loop itself already retries forever, so a reboot only needs
> the process to be started again.

---

## Component behavior

| Component | What the operator sees |
| --------- | ---------------------- |
| Control link | One outbound TCP connection from the node to `server:7000`. |
| FRP session | Node listens on `local_port` (check `ss -tlnp \| grep tun`). |
| P2P session | Node holds a UDP socket; constant keepalive traffic to the target. |
| P2P hub | One UDP socket registered with `server:7001`; a `direct` link on the dashboard means the punch completed. |
| Link test | Every few seconds each side sends a small packet the peer echoes back; the dashboard shows the round trip in milliseconds and marks the link `no answer` when echoes stop. |
| After Reset  | All listeners/sockets from FRP, TRP, P2P and the hub are gone; node back to idle. |

Toggling is seamless: sending a new `0x01`/`0x02` replaces the previous FRP or P2P session
automatically, and a `0x00` always stops everything.

### Link test

A hole punch completing is not the same as two devices being able to talk. The punch is
one packet in one direction, and it can succeed against a peer whose path has since
changed; the client keeps reporting `direct` in that case, because that is what the
punch said. The link test is the separate check.

Once a peer socket is up, each side sends a 12-byte packet every three seconds:

```text
'P','1','T' | node_id(8) | seq(1)
```

The peer echoes it back byte for byte. Only the echo of the sequence that is
outstanding counts, which is what makes it a measurement rather than a receipt: a
stale echo of an earlier test cannot pass for the current one, and a probe echo
cannot pass for a test at all. The round trip is timed on a monotonic clock, so an
NTP step cannot invent latency.

The result travels up in the existing status report, inside the flags byte that
already carried `direct`:

| Bit | Meaning |
| --- | ------- |
| 0 | a punch landed and the peer answered |
| 1 | a link test completed |
| 2-5 | round trip, in four bits of latency buckets |

Everything is additive inside that one byte, so a client and a server from
different releases still read each other's reports: a client too old to test
reports bit 0 alone, which the server reads as "punched, never measured" and shows
as `no answer` rather than as a link that works.

A test that gets no echo within two seconds clears the result. That is what keeps a
link which dies between two tests from keeping its old round trip on the dashboard.

---

## Build

`make` builds `tun` for your host machine. The Makefile also exposes per-platform targets:
`make linux`, `make windows`, `make macos`, `make android`, or `make release` for all of them.

`make test` builds and runs the link test against a fake peer over loopback. It exercises the
real `p2p_thread`, so it is the check for anything touching hole punching, echoing or the
link-test result — it includes `tun.c` directly and is never shipped in a release.

### Release build (all platforms at once)

The repo ships `.github/workflows/release.yml`. Push a tag and GitHub Actions builds every
platform, attaching each binary plus its `.sha256` checksum to the Release:

```sh
git tag v1.0.0
git push origin v1.0.0
```

Or run the workflow manually (Actions tab → "release" → Run workflow): everything is uploaded as
build artifacts, and attaching to a Release requires a tag push.

Supported binaries:

```
Linux:    tun_linux_x86_64   tun_linux_arm64
macOS:    tun_macos_x86_64   tun_macos_arm64
Windows:  tun_windows_x86_64.exe   tun_windows_i686.exe
Android:  tun_android_arm64-v8a   tun_android_armeabi-v7a   tun_android_x86_64
```

On Windows, run via `tun.exe <control-server-ip> <psk>`. A statically linked x86_64 Linux build
runs on any x86_64 Linux; `tun_linux_arm64` is for ARM64 Linux machines and phones.

**Not routers.** Consumer routers (RouterOS, OpenWrt, and friends) are deliberately not a target:
this client is a plain socket relay and never creates a `tun0` interface, and sideloading a binary
onto locked-down router firmware is not something that can be automated. Point a router at the
control server from a supported host instead.

### Android

Android builds go through the **NDK**, so the binary links against **bionic** — never glibc. CI
unpacks NDK `r27c` (SDK version `27.2.12479018`) straight from `dl.google.com` — no `sdkmanager`,
no JDK, no licence prompt — and calls the same `make android` target you can run locally:

```sh
export ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/27.2.12479018
make android
```

ABIs are `arm64-v8a`, `armeabi-v7a` and `x86_64`, all at **minSdk 21** (Android 5.0+). Keep the target
API at or below the oldest OS version you intend to support, or you will reference bionic symbols
that device simply does not have.

The binaries are PIE and dynamically linked against the device's own `libc.so`. That is deliberate:
bionic refuses to run *statically* linked executables on ARM/ARM64 below API 29
(`executable's TLS segment is underaligned`), and every device already ships the loader
(`/system/bin/linker64`) and libc, so there is nothing extra to bundle.

#### Running it on a device

Android's W^X and SELinux policies mean a binary **cannot** be executed from the SD card or shared
storage (`/sdcard`, `/storage/emulated`). It has to live in app-private storage or a dedicated
environment like Termux.

Via adb (developers):

```sh
adb push tun_android_arm64-v8a /data/local/tmp/tun
adb shell chmod +x /data/local/tmp/tun
adb shell /data/local/tmp/tun <control-server-ip> <psk>
```

Via Termux: download the binary, move it into `~/`, then `chmod +x tun` and `./tun <ip> <psk>`.

For general consumers the Play Store will not take a raw binary — it has to be wrapped in an app.
Put the matching ABI's binary in `src/main/jniLibs/<abi>/` (or in `assets/`), then at first launch
copy it out to `context.getFilesDir()`, `chmod +x` it, and start it with `ProcessBuilder` or
`fork()`/`exec()` through JNI. A rooted device gets the same result more simply: the client installs
`/data/adb/service.d/tun.sh` for Magisk on its first run.

---

## Local smoke test (FRP)

Simulate a control server, an echo service, and drive the client — no real network needed:

```python
import os, socket, struct, threading, time, subprocess

def serve_echo():
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 9001)); s.listen(5)
    c, _ = s.accept(); data = c.recv(1024); c.sendall(b"ECHO:" + data); c.close()

threading.Thread(target=serve_echo, daemon=True).start()

# TUN_FOREGROUND=1 keeps the client in the foreground so this script owns it
# and can kill it; without it the client double-forks into a daemon.
proc = subprocess.Popen(["./tun", "127.0.0.1", "secretkey99"],
                        env={**os.environ, "TUN_FOREGROUND": "1"})

ctrl = socket.socket(); ctrl.bind(("127.0.0.1", 7000)); ctrl.listen(1)
conn, _ = ctrl.accept()
print("psk received:", conn.recv(64))
# FRP: local port 9000 -> server port 9001. The payload is 8 bytes (two
# big-endian ports + 4 unused), then the 8-byte node-id field, which is '0'
# padding for commands that name no peer: 1 + 8 + 8 = 17 bytes total.
conn.sendall(struct.pack("B", 0x01) + struct.pack(">H", 9000) + struct.pack(">H", 9001)
             + b"\x00" * 4 + b"0" * 8)

time.sleep(1)
s = socket.create_connection(("127.0.0.1", 9000), timeout=5)
s.sendall(b"ping")
print("reply:", s.recv(1024))          # expect: b"ECHO:ping"
```

**Note the frame length.** A control frame is exactly 17 bytes: 1 command + 8 payload + 8 node-id.
Sending only the 10 payload bytes desynchronizes the stream, and the client will read your next
command shifted by seven bytes — which usually looks like a client that ignores the dashboard.

---

## Notes & limitations

- **Auth**: the PSK is sent in cleartext over the control channel. On hostile networks, wrap the
  control link in a VPN or encrypt at a higher layer. The connection is always outbound, so NAT /
  firewalls don't block the initial handshake. It is also stored on disk in the state directory
  (as `connection`, mode `0600`) so the client can restart itself after a reboot — see
  [Survives a reboot](#survives-a-reboot).
- **Linux / macOS / Windows / Android.** Linux binaries are statically linked (musl); macOS and
  Windows use their native toolchains (Apple clang, MinGW-w64); Android uses the NDK against bionic.
  The client daemonizes on Linux/macOS/Android but runs in the foreground on Windows unless
  `TUN_FOREGROUND=1` is set. Kernel 3.2+ for the Linux builds; no libc or framework dependencies
  thanks to static musl builds.
- **BSD** is not in the release matrix.
- **Routers are not a target** — see the note in [Build](#build). The client relays sockets; it never
  creates a `tun0` interface, so a router OS that requires one to be present gets nothing.
- No runtime state beyond the two files in the state directory (`connection` and `device_id`): the
  orchestrator remains the only source of truth for what the pipelines should be doing.