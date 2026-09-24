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

The client immediately forks into the background and returns control to your shell. The control
server receives your PSK, then drives the client with 9-byte command blocks.

| Byte 0 (command) | Meaning                    | Payload (8 bytes)                        |
| ---------------- | -------------------------- | ---------------------------------------- |
| `0x00`           | **Reset / Idle**           | (none) — kills active FRP & P2P sessions |
| `0x01`           | **FRP Reverse Proxy**      | `local_port` (2B, big-endian) + `remote_port` (2B, big-endian) |
| `0x02`           | **P2P Mesh / UDP NAT hole-punch** | `target_ip` (4 raw bytes) + `target_port` (2B, big-endian) |

- **FRP**: the client listens on `local_port`; every incoming connection is forwarded over a new
  TCP socket to `<control-server-ip>:<remote_port>`.
- **P2P**: the client opens a UDP socket and starts STUN-style hole-punching against
  `target_ip:target_port`, sending keepalives and echoing anything that comes back so the NAT
  mapping stays open.
- **Reset**: tears down both active pipelines and returns to idle. Payload is ignored.

---

## Is it running?

The client is silent by design (no pidfile, no stdout, no logs), so check it 3 ways:

```sh
# 1. Process is alive
pgrep -laf tun
# or
ps -ef | grep [t]un

# 2. Live control connection to your server:7000
ss -tnp | grep tun
# or
netstat -tnp | grep tun
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

### Native build (one machine)

```sh
gcc -Os -ffunction-sections -fdata-sections -Wl,--gc-sections -static -s tun.c -o tun
```

### Cross-compile (needs the matching musl cross-gcc installed)

```sh
make            # x86_64 (default)
make tun_mips   # MIPS routers
make tun_arm64  # ARM64 (RouterOS / OpenWrt)
```

### Release build (all machines at once)

The repo ships `.github/workflows/release.yml`. Push a tag and GitHub Actions cross-compiles
fully static binaries for every supported architecture, attaching each binary plus its `.sha256`
checksum to the Release:

```sh
git tag v1.0.0
git push origin v1.0.0
```

Or run the workflow manually (Actions tab → "release" → Run workflow): everything is uploaded as
build artifacts, and attaching to a Release requires a tag push.

Supported architectures (all Linux, musl-libc statically linked):

```
x86_64  i686  arm64  armv7  armv6  mips  mipsel  mips64
powerpc powerpc64 riscv64 s390x m68k sh4
```

Pick the binary that matches the target machine's CPU. A statically linked x86_64 build runs on
any x86_64 Linux, but it will **not** run on an ARM/MIPS device — use that device's variant.

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
  firewalls don't block the initial handshake.
- **Linux only.** Kernel 3.2+; no libc or framework dependencies thanks to static musl builds.
- **Windows / macOS / BSD** are not targeted by the current source or the release matrix.
- `loongarch64` is not available as a prebuilt musl toolchain, so it is not in the release matrix.
- No persistence: nothing writes config or state anywhere on the node; the orchestrator is the
  only source of truth. If the machine reboots, re-run the binary.