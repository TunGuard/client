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
on Windows keep it running yourself, e.g. as a scheduled task). The control
server receives your PSK, then drives the client with 9-byte command blocks.

| Byte 0 (command) | Meaning                    | Payload (8 bytes)                        |
| ---------------- | -------------------------- | ---------------------------------------- |
| `0x00`           | **Reset / Idle**           | (none) — kills active FRP, TRP & P2P sessions |
| `0x01`           | **FRP Reverse Proxy**      | `local_port` (2B, big-endian) + `remote_port` (2B, big-endian) |
| `0x02`           | **P2P Mesh / UDP NAT hole-punch** | `target_ip` (4 raw bytes) + `target_port` (2B, big-endian) |
| `0x03`           | **TRP Pull Reverse Proxy** | `target_ip` (4 raw bytes) + `service_port` (2B) + `public_port` (2B, big-endian) |

- **FRP**: the client listens on `local_port`; every incoming connection is forwarded over a new
  TCP socket to `<control-server-ip>:<remote_port>`.
- **P2P**: the client opens a UDP socket and starts STUN-style hole-punching against
  `target_ip:target_port`, sending keepalives and echoing anything that comes back so the NAT
  mapping stays open.
- **TRP (pull)**: the server (on behalf of a user who pinned a public port on the VPS) tells the
  client to dial back `<control-server-ip>:<public_port>` **and** its own local service
  (`target_ip:service_port`, `0.0.0.0` → `127.0.0.1`), then pumps the two. One session per command.
- **Reset**: tears down all active pipelines and returns to idle. Payload is ignored.

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
check.

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

**Saved config.** On a normal start the client writes `tun.conf` *next to the binary* (mode `0600`),
and started with no arguments it reads that file back:

```ini
server=156.232.88.212
psk=secretkey99
```

This is the same "no local state" design as everything else — the file holds only the two values you
already typed on the command line. Delete it to wipe it. `TUN_CONFIG=/path/to/file ./tun` puts it
somewhere else (handy for read-only install dirs).

**Auto-start.** When saved successfully and the process is root/admin, it also installs a boot hook.
Each backend is best effort and silently skipped when it does not apply:

| Platform     | Boot hook                                                                              |
| ------------ | -------------------------------------------------------------------------------------- |
| Linux        | `/etc/systemd/system/tun.service` + `systemctl enable` (only if systemd is present)      |
| Windows      | `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run\tun` (needs an elevated shell)       |
| Android      | `/data/adb/service.d/tun.sh` (Magisk runs this as root at boot)                          |

```sh
# check what got installed
systemctl status tun          # Linux
reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run" /v tun   # Windows
ls -l /data/adb/service.d/tun.sh                                          # Android
```

**Foreground mode.** Boot hooks supervise the process directly, so when `TUN_FOREGROUND=1` is set the
client skips the double-fork and stays in the foreground. That is what the systemd unit sets; a
manual `./tun <ip> <psk>` still backgrounds as before.

> Re-running `./tun <ip> <psk>` re-points the client at a new server and rewrites both the config and
> the boot hook. The connect loop itself already retries forever, so a reboot only needs the process
> to be started again.

---

## Component behavior

| Component | What the operator sees |
| --------- | ---------------------- |
| Control link | One outbound TCP connection from the node to `server:7000`. |
| FRP session | Node listens on `local_port` (check `ss -tlnp \| grep tun`). |
| P2P session | Node holds a UDP socket; constant keepalive traffic to the target. |
| After Reset  | All listeners/sockets from FRP and P2P are gone; node back to idle. |

Toggling is seamless: sending a new `0x01`/`0x02` replaces the previous FRP or P2P session
automatically, and a `0x00` always stops everything.

---

## Build

`make` builds `tun` for your host machine. The Makefile also exposes per-platform targets:
`make linux`, `make windows`, `make macos`, `make android`, or `make release` for all of them.

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
installs NDK `27.2.12479018` with `sdkmanager` and calls the same `make android` target you can run
locally:

```sh
export ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/27.2.12479018
make android
```

ABIs are `arm64-v8a`, `armeabi-v7a` and `x86_64`, all at **minSdk 21** (Android 5.0+).

The binaries are PIE and dynamically linked against the device's own `libc.so`. That is deliberate:
bionic refuses to run *statically* linked executables on ARM/ARM64 below API 29
(`executable's TLS segment is underaligned`), and every device already ships the loader
(`/system/bin/linker64`) and libc, so there is nothing extra to bundle. Deploy by dropping the file
somewhere executable — `/data/local/tmp` is the usual place.

---

## Local smoke test (FRP)

Simulate a control server, an echo service, and drive the client — no real network needed:

```python
import socket, struct, threading, time, subprocess

def serve_echo():
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 9001)); s.listen(5)
    c, _ = s.accept(); data = c.recv(1024); c.sendall(b"ECHO:" + data); c.close()

threading.Thread(target=serve_echo, daemon=True).start()

proc = subprocess.Popen(["./tun", "127.0.0.1", "secretkey99"])

ctrl = socket.socket(); ctrl.bind(("127.0.0.1", 7000)); ctrl.listen(1)
conn, _ = ctrl.accept()
print("psk received:", conn.recv(64))
# FRP: local port 9000 -> server port 9001
conn.sendall(struct.pack("B", 0x01) + struct.pack(">H", 9000) + struct.pack(">H", 9001) + b"\x00\x00\x00\x00")

time.sleep(1)
s = socket.create_connection(("127.0.0.1", 9000), timeout=5)
s.sendall(b"ping")
print("reply:", s.recv(1024))          # expect: b"ECHO:ping"
```

---

## Notes & limitations

- **Auth**: the PSK is sent in cleartext over the control channel. On hostile networks, wrap the
  control link in a VPN or encrypt at a higher layer. The connection is always outbound, so NAT /
  firewalls don't block the initial handshake. It is also stored on disk in `tun.conf` (mode `0600`)
  so the client can restart itself after a reboot — see
  [Survives a reboot](#survives-a-reboot).
- **Linux / macOS / Windows / Android.** Linux binaries are statically linked (musl); macOS and
  Windows use their native toolchains (Apple clang, MinGW-w64); Android uses the NDK against bionic.
  The client daemonizes on Linux/macOS/Android but runs in the foreground on Windows unless
  `TUN_FOREGROUND=1` is set. Kernel 3.2+ for the Linux builds; no libc or framework dependencies
  thanks to static musl builds.
- **BSD** is not in the release matrix.
- **Routers are not a target** — see the note in [Build](#build). The client relays sockets; it never
  creates a `tun0` interface, so a router OS that requires one to be present gets nothing.
- No runtime state beyond `tun.conf`: the orchestrator remains the only source of truth for what the
  pipelines should be doing.