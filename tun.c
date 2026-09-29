#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <direct.h>
#include <process.h>
typedef SOCKET sock_t;
#define CLOSE_SOCK(s) closesocket(s)
#define SLEEP_MS(ms) Sleep(ms)
#define MKDIR(p) _mkdir(p)
typedef unsigned (__stdcall *thread_func)(void *);
typedef HANDLE thread_t;
static int thread_create(thread_t *t, thread_func f, void *arg)
{
    *t = (HANDLE)_beginthreadex(NULL, 0, f, arg, 0, NULL);
    return *t ? 0 : -1;
}
static void thread_join(thread_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
static void thread_detach(thread_t t) { if (t) CloseHandle(t); }
#define THREAD_FN(name) static unsigned __stdcall name(void *arg)
#define THREAD_EXIT return 0
typedef CRITICAL_SECTION mutex_t;
static void mutex_init(mutex_t *m) { InitializeCriticalSection(m); }
static void mutex_lock(mutex_t *m) { EnterCriticalSection(m); }
static void mutex_unlock(mutex_t *m) { LeaveCriticalSection(m); }
static void sleep_sec(unsigned s) { Sleep(s * 1000); }
static unsigned long rand_seed(void) { return (unsigned long)GetTickCount() ^ (unsigned long)GetCurrentProcessId(); }
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define CLOSE_SOCK(s) close(s)
#define SLEEP_MS(ms) usleep((ms) * 1000)
#define MKDIR(p) mkdir((p), 0700)
typedef void *(*thread_func)(void *);
typedef pthread_t thread_t;
static int thread_create(thread_t *t, thread_func f, void *arg)
{
    return pthread_create(t, NULL, f, arg);
}
static void thread_join(thread_t t) { pthread_join(t, NULL); }
static void thread_detach(thread_t t) { pthread_detach(t); }
#define THREAD_FN(name) static void *name(void *arg)
#define THREAD_EXIT return NULL
typedef pthread_mutex_t mutex_t;
static void mutex_init(mutex_t *m) { pthread_mutex_init(m, NULL); }
static void mutex_lock(mutex_t *m) { pthread_mutex_lock(m); }
static void mutex_unlock(mutex_t *m) { pthread_mutex_unlock(m); }
static void sleep_sec(unsigned s) { sleep(s); }
static unsigned long rand_seed(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long)tv.tv_sec ^ ((unsigned long)tv.tv_usec << 8) ^ (unsigned long)getpid();
}
#endif

#define CMD_RESET    0x00
#define CMD_FRP      0x01
#define CMD_P2P      0x02
#define CMD_TRP      0x03
#define CMD_P2P_ADD  0x04
#define CMD_P2P_DEL  0x05
#define CMD_P2P_CLR  0x06
#define CMD_P2P_HUB  0x07
#define CMD_HUB_STOP 0x08

/* First byte of a client -> server status report. The server never sends this
 * value, so a control frame starting with 0xFE is unambiguously a report. */
#define STATUS_BYTE  0xFE

#define CTRL_PORT    7000
#define RELAY_BUF    8192
#define P2P_MAX      8
#define PATH_MAX_LEN 128
#define ID_LEN       8

#define CTRL_PORT_FILE_MAX 64

/* Control frames are a fixed 17 bytes:
 *   [0]      command
 *   [1..8]   command payload
 *   [9..16]  node id, 8 ASCII hex chars ('0' when the command has no peer)
 *
 * The node id is what lets several devices share one PSK: the server hands a
 * node its own id in the CMD_P2P_HUB frame, and every later command that names
 * a peer carries that peer's id instead of a positional slot.
 */
#define FRAME_SIZE   17

/* TRP pull-mode reverse proxy payload (8 bytes):
 *   [0..3] target IPv4 (raw), 0 = 127.0.0.1
 *   [4..5] service port (big-endian) — port of the local service
 *   [6..7] public port   (big-endian) — server port the client dials back
 *
 * P2P peer payload (8 bytes), for CMD_P2P_ADD and CMD_P2P_HUB:
 *   [0..3] target IPv4 (raw)
 *   [4..5] target port  (big-endian)
 *   [6..7] unused
 *
 * BYTE ORDER: ports are big-endian, and the address bytes are stored exactly
 * as they sit on the wire (network order), because they are copied straight
 * into sin_addr.s_addr. Read them with rd16()/rd_ip() rather than re-shifting
 * them into a host-order integer: doing that by hand byte-swaps the address,
 * so 127.0.0.1 becomes 1.0.0.127 and the client punches at a black hole while
 * looking perfectly healthy.
 *
 * Client -> server status report:
 *   [0]      0xFE
 *   [1]      peer count
 *   then count x { node id (8 ASCII hex), flags (bit 0 = direct path open) }
 *
 * Hub datagrams. Every one starts with a 3-byte magic and the sender's node id
 * so the server can attribute it without guessing from a source address —
 * several devices behind one NAT share a single public IP.
 *   'T','U','N' | node_id(8) | counter(1)                  keepalive probe
 *   'P','1','H' | node_id(8)                               rendezvous request
 *   'P','1','R' | count(1) | count x { node_id(8) | ip(4) | port(2) }
 *
 * The hub socket is deliberately NOT connect()ed. A connected UDP socket only
 * accepts datagrams from its peer, which is the server relay — so the very
 * punch that completes a direct path would be dropped by the kernel and no
 * link could ever go direct. Using sendto/recvfrom keeps the NAT mapping open
 * while letting a peer's punch land on the same socket; such a packet is what
 * proves the path open, and is echoed back so both ends agree.
 */

#define HUB_PROBE_MAGIC "TUN"
#define HUB_HELLO_MAGIC "P1H"
#define HUB_REPLY_MAGIC "P1R"

struct ControlPacket {
    uint8_t command;
    uint8_t payload[8];
    char node_id[ID_LEN];   /* not NUL terminated: this is a wire frame */
};

/* The wire format is positional, so the struct has to be exactly FRAME_SIZE. */
typedef char ctrl_frame_size_check[(sizeof(struct ControlPacket) == FRAME_SIZE) ? 1 : -1];

static volatile int g_frp_stop = 0;
static volatile int g_trp_stop = 0;

static mutex_t g_p2p_mu;
static struct p2p_peer *g_peers[P2P_MAX];
static int g_p2p_ready = 0;

/* The control socket is owned by the main thread; the status reporter writes
 * to it from its own thread, so the descriptor and its writes are guarded. */
static sock_t g_ctrl_fd = INVALID_SOCKET;
static mutex_t g_ctrl_mu;

#ifndef _WIN32
static void daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);
    if (setsid() < 0) exit(EXIT_FAILURE);
    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);
    umask(0);
    if (chdir("/") != 0) exit(EXIT_FAILURE);
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}
#endif

static int read_full(sock_t fd, void *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int n = recv(fd, (char *)buf + off, (int)(len - off), 0);
#ifndef _WIN32
        if (n < 0 && errno == EINTR) continue;
#endif
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static void write_full(sock_t fd, const void *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int n = send(fd, (const char *)buf + off, (int)(len - off), 0);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

static void pump(sock_t a, sock_t b, const volatile int *stop)
{
    fd_set fds;
    char buf[RELAY_BUF];
    while (!*stop) {
        FD_ZERO(&fds);
        FD_SET(a, &fds);
        FD_SET(b, &fds);
        int m = a > b ? (int)a : (int)b;
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        if (select(m + 1, &fds, NULL, NULL, &tv) < 0) break;
        if (*stop) break;
        if (FD_ISSET(a, &fds)) {
            int n = recv(a, buf, (int)sizeof(buf), 0);
            if (n <= 0) break;
            if (send(b, buf, (int)n, 0) <= 0) break;
        }
        if (FD_ISSET(b, &fds)) {
            int n = recv(b, buf, (int)sizeof(buf), 0);
            if (n <= 0) break;
            if (send(a, buf, (int)n, 0) <= 0) break;
        }
    }
}

/* ---- FRP (0x01) ---------------------------------------------------------- */

struct frp_arg {
    char server_ip[16];
    uint16_t local_port;
    uint16_t remote_port;
};

struct frp_conn {
    sock_t c;
    char server_ip[16];
    uint16_t remote_port;
};

THREAD_FN(frp_conn_handler)
{
    struct frp_conn *fc = (struct frp_conn *)arg;
    struct sockaddr_in rsa;
    sock_t rs = socket(AF_INET, SOCK_STREAM, 0);
    memset(&rsa, 0, sizeof(rsa));
    rsa.sin_family = AF_INET;
    rsa.sin_port = htons(fc->remote_port);
    rsa.sin_addr.s_addr = inet_addr(fc->server_ip);
    if (rs == INVALID_SOCKET ||
        connect(rs, (struct sockaddr *)&rsa, sizeof(rsa)) < 0) {
        if (rs != INVALID_SOCKET) CLOSE_SOCK(rs);
        CLOSE_SOCK(fc->c);
        free(fc);
        THREAD_EXIT;
    }
    pump(fc->c, rs, &g_frp_stop);
    CLOSE_SOCK(fc->c);
    CLOSE_SOCK(rs);
    free(fc);
    THREAD_EXIT;
}

THREAD_FN(frp_session)
{
    struct frp_arg *fa = (struct frp_arg *)arg;
    struct sockaddr_in lsa;
    sock_t lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock == INVALID_SOCKET) { free(fa); THREAD_EXIT; }
    int one = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    memset(&lsa, 0, sizeof(lsa));
    lsa.sin_family = AF_INET;
    lsa.sin_addr.s_addr = htonl(INADDR_ANY);
    lsa.sin_port = htons(fa->local_port);
    if (bind(lsock, (struct sockaddr *)&lsa, sizeof(lsa)) < 0 ||
        listen(lsock, 16) < 0) {
        CLOSE_SOCK(lsock);
        free(fa);
        THREAD_EXIT;
    }
    while (!g_frp_stop) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(lsock, &r);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        if (select((int)lsock + 1, &r, NULL, NULL, &tv) < 0) break;
        if (g_frp_stop) break;
        if (!FD_ISSET(lsock, &r)) continue;
        sock_t c = accept(lsock, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        struct frp_conn *fc = (struct frp_conn *)malloc(sizeof(*fc));
        if (!fc) { CLOSE_SOCK(c); continue; }
        fc->c = c;
        memcpy(fc->server_ip, fa->server_ip, sizeof(fc->server_ip));
        fc->remote_port = fa->remote_port;
        thread_t h;
        if (thread_create(&h, frp_conn_handler, fc) != 0) {
            CLOSE_SOCK(c);
            free(fc);
            continue;
        }
        thread_detach(h);
    }
    CLOSE_SOCK(lsock);
    free(fa);
    THREAD_EXIT;
}

/* ---- P2P mesh (0x02 / 0x04 / 0x05 / 0x06 / 0x07) ------------------------- */
/*
 * Two kinds of socket, both UDP, both keeping a NAT mapping open:
 *
 *  - One hub socket (CMD_P2P_HUB). It sends a "TUN" probe every second so the
 *    server can register this node's mapped endpoint, and a "P1H" rendezvous
 *    request every other second. The server answers a P1H with the endpoints of
 *    every online peer sharing our PSK, which is how a device discovers the
 *    others without anybody touching the dashboard. A "P1R" reply is the only
 *    thing the hub socket acts on.
 *
 *  - One socket per discovered peer (CMD_P2P_ADD). Connected to the peer's
 *    observed endpoint, so the packet it sends opens the mapping the peer is
 *    already aiming at. Receiving anything at all on this socket proves the
 *    direct path is open, because a hub-bound probe never reaches a peer
 *    socket. Probes are echoed so both ends converge on the same conclusion;
 *    anything else is echoed unchanged, preserving the original link semantics.
 *
 * Receiving a probe is itself the punch completing, so both sides mark
 * themselves direct from the same exchange and report it upward. A punch can
 * also land on the hub socket instead, which is why that socket is
 * unconnected and echoes non-reply datagrams; see the protocol notes above.
 */

struct p2p_peer {
    char id[ID_LEN + 1];
    struct sockaddr_in target;
    volatile int running;
    volatile int direct;
    thread_t thr;
};

THREAD_FN(p2p_thread)
{
    struct p2p_peer *p = (struct p2p_peer *)arg;
    unsigned char probe[3 + ID_LEN + 1];
    unsigned char buf[2048];
    memcpy(probe, HUB_PROBE_MAGIC, 3);
    memcpy(probe + 3, p->id, ID_LEN);
    probe[3 + ID_LEN] = 0x00;

    /* Retries for as long as the slot is live: a peer that is not reachable
     * yet (its NAT has not been punched) is the normal case, not a failure.
     * This struct is owned by p2p_stop, which joins us before freeing. */
    while (p->running) {
        sock_t u = socket(AF_INET, SOCK_DGRAM, 0);
        if (u == INVALID_SOCKET) { SLEEP_MS(1000); continue; }
        if (connect(u, (struct sockaddr *)&p->target, sizeof(p->target)) < 0) {
            CLOSE_SOCK(u);
            SLEEP_MS(2000);
            continue;
        }
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500000;
        setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
        while (p->running) {
            int n = 0;
#ifndef _WIN32
            do {
                n = recv(u, buf, (int)sizeof(buf), 0);
            } while (n < 0 && errno == EINTR && p->running);
#else
            n = recv(u, buf, (int)sizeof(buf), 0);
#endif
            if (n > 0) {
                /* Anything landing here arrived straight from the peer. */
                p->direct = 1;
                send(u, buf, (int)n, 0);
            }
            send(u, probe, (int)sizeof(probe), 0);
            probe[3 + ID_LEN]++;
            SLEEP_MS(1000);
        }
        CLOSE_SOCK(u);
    }
    p->running = 0;
    THREAD_EXIT;
}

/* Stops the peer with this id and joins its thread. Safe for an unknown id.
 * The table entry is cleared first so the status reporter never observes a
 * thread that is on its way out. */
static void p2p_stop(const char *id)
{
    struct p2p_peer *p = NULL;
    int i;
    if (!g_p2p_ready) return;
    mutex_lock(&g_p2p_mu);
    for (i = 0; i < P2P_MAX; i++) {
        if (g_peers[i] && memcmp(g_peers[i]->id, id, ID_LEN) == 0) {
            p = g_peers[i];
            g_peers[i] = NULL;
            break;
        }
    }
    mutex_unlock(&g_p2p_mu);
    if (!p) return;
    p->running = 0;
    thread_join(p->thr);
    free(p);
}

/* Stops whatever occupies a table index. Joining one thread at a time, with
 * the slot released first, is what keeps a concurrent p2p_start from
 * overwriting a table entry whose thread is still running. */
static void p2p_stop_slot_at(int idx)
{
    char id[ID_LEN + 1];
    if (!g_p2p_ready) return;
    mutex_lock(&g_p2p_mu);
    if (g_peers[idx]) {
        memcpy(id, g_peers[idx]->id, ID_LEN);
        id[ID_LEN] = '\0';
    } else {
        id[0] = '\0';
    }
    mutex_unlock(&g_p2p_mu);
    if (id[0]) p2p_stop(id);
}

static void p2p_clear(void)
{
    int i;
    for (i = 0; i < P2P_MAX; i++) p2p_stop_slot_at(i);
}

/* Starts (or restarts) the punch toward one peer. Re-sending the same target
 * for a peer that is already running is a no-op, so a periodic re-push from
 * the server never disturbs a link that is already up. */
static void p2p_start(const char *id, uint32_t ip, uint16_t port)
{
    int idx = -1;
    int i;
    if (!g_p2p_ready || !id || id[0] == '\0') return;

    mutex_lock(&g_p2p_mu);
    for (i = 0; i < P2P_MAX; i++) {
        if (!g_peers[i]) {
            if (idx < 0) idx = i;
            continue;
        }
        if (memcmp(g_peers[i]->id, id, ID_LEN) == 0) {
            if (g_peers[i]->target.sin_addr.s_addr == ip &&
                g_peers[i]->target.sin_port == htons(port)) {
                mutex_unlock(&g_p2p_mu);
                return;   /* already punching exactly this target */
            }
            idx = i;      /* target moved: restart this entry */
            break;
        }
    }
    mutex_unlock(&g_p2p_mu);

    if (idx < 0) return;   /* table full; the server caps this at P2P_MAX */

    if (g_peers[idx]) {
        char victim[ID_LEN + 1];
        mutex_lock(&g_p2p_mu);
        memcpy(victim, g_peers[idx]->id, ID_LEN);
        victim[ID_LEN] = '\0';
        mutex_unlock(&g_p2p_mu);
        p2p_stop(victim);
    }

    struct p2p_peer *p = (struct p2p_peer *)calloc(1, sizeof(*p));
    if (!p) return;
    memcpy(p->id, id, ID_LEN);
    p->id[ID_LEN] = '\0';
    p->target.sin_family = AF_INET;
    p->target.sin_addr.s_addr = ip;
    p->target.sin_port = htons(port);
    p->running = 1;
    mutex_lock(&g_p2p_mu);
    g_peers[idx] = p;
    mutex_unlock(&g_p2p_mu);
    thread_t thr;
    if (thread_create(&thr, p2p_thread, p) != 0) {
        mutex_lock(&g_p2p_mu);
        g_peers[idx] = NULL;
        mutex_unlock(&g_p2p_mu);
        free(p);
    }
}

/* ---- Rendezvous hub socket (CMD_P2P_HUB) ---------------------------------- */

static char g_node_id[ID_LEN + 1];
static volatile int g_hub_running = 0;
static struct sockaddr_in g_hub_addr;
static thread_t g_hub_thr;
static int g_hub_ready = 0;

/* Marks the link to a known peer as direct once that peer's punch has actually
 * arrived, so the status reporter can carry the bit upward. */
static void p2p_mark_direct_by_id(const char *id)
{
    int i;
    if (!id || id[0] == '\0') return;
    mutex_lock(&g_p2p_mu);
    for (i = 0; i < P2P_MAX; i++) {
        if (g_peers[i] && memcmp(g_peers[i]->id, id, ID_LEN) == 0) g_peers[i]->direct = 1;
    }
    mutex_unlock(&g_p2p_mu);
}

static void parse_rendezvous_reply(const unsigned char *buf, int n)
{
    int off;
    int count;
    if (n < 4 || memcmp(buf, HUB_REPLY_MAGIC, 3) != 0) return;
    count = buf[3];
    off = 4;
    while (count-- > 0 && off + ID_LEN + 6 <= n) {
        char id[ID_LEN + 1];
        uint16_t port;
        memcpy(id, buf + off, ID_LEN);
        id[ID_LEN] = '\0';
        port = (uint16_t)((buf[off + ID_LEN + 4] << 8) | buf[off + ID_LEN + 5]);
        /* A node is never its own peer. */
        if (memcmp(id, g_node_id, ID_LEN) != 0) {
            uint32_t ip;
            memcpy(&ip, buf + off + ID_LEN, 4);
            p2p_start(id, ip, port);
        }
        off += ID_LEN + 6;
    }
}

THREAD_FN(hub_thread)
{
    unsigned char probe[3 + ID_LEN + 1];
    unsigned char hello[3 + ID_LEN];
    unsigned char buf[2048];
    unsigned int tick = 0;
    (void)arg;
    memcpy(probe, HUB_PROBE_MAGIC, 3);
    memcpy(probe + 3, g_node_id, ID_LEN);
    probe[3 + ID_LEN] = 0x00;
    memcpy(hello, HUB_HELLO_MAGIC, 3);
    memcpy(hello + 3, g_node_id, ID_LEN);

    while (g_hub_running) {
        sock_t u = socket(AF_INET, SOCK_DGRAM, 0);
        if (u == INVALID_SOCKET) { SLEEP_MS(1000); continue; }
        /* Deliberately not connect()ed. A connected UDP socket only accepts
         * datagrams from the relay, which is exactly the peer punch that
         * completes the direct path — the kernel would drop it and the link
         * could never go direct. sendto/recvfrom keeps the NAT mapping while
         * letting a peer's punch land here. */
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500000;
        setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
        while (g_hub_running) {
            struct sockaddr_in from;
            socklen_t flen = sizeof(from);
            int n = 0;
#ifndef _WIN32
            do {
                n = (int)recvfrom(u, buf, (int)sizeof(buf), 0,
                                  (struct sockaddr *)&from, &flen);
            } while (n < 0 && errno == EINTR && g_hub_running);
#else
            n = (int)recvfrom(u, buf, (int)sizeof(buf), 0,
                              (struct sockaddr *)&from, &flen);
#endif
            if (n > 0) {
                if (n >= 4 && memcmp(buf, HUB_REPLY_MAGIC, 3) == 0) {
                    parse_rendezvous_reply(buf, n);
                } else {
                    /* A peer punched at this socket: receiving anything proves
                     * its path to us is open, so record it and echo the packet
                     * back so the far end reaches the same conclusion. */
                    char id[ID_LEN + 1];
                    if (n >= 3 + ID_LEN) {
                        memcpy(id, buf + 3, ID_LEN);
                        id[ID_LEN] = '\0';
                    } else {
                        id[0] = '\0';
                    }
                    p2p_mark_direct_by_id(id);
                    sendto(u, buf, n, 0, (struct sockaddr *)&from, sizeof(from));
                }
            }
            sendto(u, probe, (int)sizeof(probe), 0,
                   (struct sockaddr *)&g_hub_addr, sizeof(g_hub_addr));
            probe[3 + ID_LEN]++;
            /* Ask who else is out there every other second; a reply is the
             * automatic discovery of every other device on our PSK. */
            if (tick % 2 == 0)
                sendto(u, hello, (int)sizeof(hello), 0,
                       (struct sockaddr *)&g_hub_addr, sizeof(g_hub_addr));
            tick++;
            SLEEP_MS(1000);
        }
        CLOSE_SOCK(u);
    }
    g_hub_running = 0;
    THREAD_EXIT;
}

static void hub_stop(void)
{
    if (!g_hub_ready) return;
    if (!g_hub_running) return;
    g_hub_running = 0;
    thread_join(g_hub_thr);
}

static void hub_start(uint32_t ip, uint16_t port)
{
    if (!g_hub_ready) return;
    hub_stop();
    memset(&g_hub_addr, 0, sizeof(g_hub_addr));
    g_hub_addr.sin_family = AF_INET;
    g_hub_addr.sin_addr.s_addr = ip;
    g_hub_addr.sin_port = htons(port);
    g_hub_running = 1;
    if (thread_create(&g_hub_thr, hub_thread, NULL) != 0) g_hub_running = 0;
}

/* ---- TRP pull reverse proxy (0x03) ---------------------------------------- */

struct trp_arg {
    char server_ip[16];
    uint32_t target_ip;
    uint16_t service_port;
    uint16_t public_port;
};

/* TRP pull-mode: the control server instructs the client to dial back the
 * server's public port AND its own local service, then pumps the two. One
 * session per command; each is a detached thread. */
THREAD_FN(trp_session)
{
    struct trp_arg *ta = (struct trp_arg *)arg;
    struct sockaddr_in sa;
    sock_t up = socket(AF_INET, SOCK_STREAM, 0);
    sock_t lo = socket(AF_INET, SOCK_STREAM, 0);
    if (up == INVALID_SOCKET || lo == INVALID_SOCKET) {
        if (up != INVALID_SOCKET) CLOSE_SOCK(up);
        if (lo != INVALID_SOCKET) CLOSE_SOCK(lo);
        free(ta);
        THREAD_EXIT;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(ta->public_port);
    sa.sin_addr.s_addr = inet_addr(ta->server_ip);
    if (connect(up, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        CLOSE_SOCK(up);
        CLOSE_SOCK(lo);
        free(ta);
        THREAD_EXIT;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(ta->service_port);
    sa.sin_addr.s_addr = ta->target_ip ? ta->target_ip : inet_addr("127.0.0.1");
    if (connect(lo, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        CLOSE_SOCK(up);
        CLOSE_SOCK(lo);
        free(ta);
        THREAD_EXIT;
    }
    pump(up, lo, &g_trp_stop);
    CLOSE_SOCK(up);
    CLOSE_SOCK(lo);
    free(ta);
    THREAD_EXIT;
}

/* ---- Status reporting ----------------------------------------------------- */

/* One report is [0xFE][count][ (node id:8, flags:1) * count ]. It is how the
 * dashboard shows which links are direct rather than still relying on the hub. */
THREAD_FN(status_thread)
{
    unsigned char report[2 + P2P_MAX * (ID_LEN + 1)];
    (void)arg;
    while (1) {
        sleep_sec(5);
        mutex_lock(&g_ctrl_mu);
        sock_t fd = g_ctrl_fd;
        if (fd == INVALID_SOCKET) {
            mutex_unlock(&g_ctrl_mu);
            break;
        }
        int n = 0;
        int i;
        mutex_lock(&g_p2p_mu);
        for (i = 0; i < P2P_MAX; i++) {
            if (!g_peers[i]) continue;
            memcpy(report + 2 + n * (ID_LEN + 1), g_peers[i]->id, ID_LEN);
            report[2 + n * (ID_LEN + 1) + ID_LEN] = g_peers[i]->direct ? 0x01 : 0x00;
            n++;
        }
        mutex_unlock(&g_p2p_mu);
        if (n > 0) {
            report[0] = STATUS_BYTE;
            report[1] = (unsigned char)n;
            write_full(fd, report, (size_t)(2 + n * (ID_LEN + 1)));
        }
        mutex_unlock(&g_ctrl_mu);
    }
    THREAD_EXIT;
}

/* ---- Stable device identity ----------------------------------------------- */
/*
 * The PSK is a shared group key, so the server needs a second value to tell
 * devices apart and to let a reconnecting device reclaim its own record: a
 * random id generated once and kept in a small state file. Without it a device
 * that restarts would appear as a brand-new node every time.
 */

static void random_hex(char *out, size_t nbytes)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[32];
    size_t got = 0;
#ifndef _WIN32
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n;
        do {
            n = read(fd, raw + got, sizeof(raw) - got);
            if (n <= 0) break;
            got += (size_t)n;
        } while (got < sizeof(raw));
        close(fd);
    }
#else
    HCRYPTPROV prov = 0;
    /* Fall back to the seeded mixer below when CryptoAPI is unavailable. */
    if (CryptAcquireContext(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        DWORD blen = (DWORD)sizeof(raw);
        if (CryptGenRandom(prov, blen, (BYTE *)raw)) got = sizeof(raw);
        CryptReleaseContext(prov, 0);
    }
#endif
    if (got < sizeof(raw)) {
        /* Mix whatever entropy the platform gives us for free. */
        unsigned long s = rand_seed();
        size_t i;
        for (i = got; i < sizeof(raw); i++) {
            s = s * 1103515245UL + 12345UL;
            raw[i] = (unsigned char)((s >> 16) & 0xFF);
        }
    }
    size_t i;
    for (i = 0; i < nbytes && i < sizeof(raw); i++) {
        out[i * 2] = hex[(raw[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[raw[i] & 0xF];
    }
    out[nbytes * 2] = '\0';
}

/* mkdir -p. MKDIR only creates one level, and the state directory is often
 * several levels deep (TUN_STATE_DIR), so walk the path making each component.
 * Already existing is success as far as we care. */
static void mkdir_p(const char *path)
{
    char tmp[CTRL_PORT_FILE_MAX * 2];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return;
    memcpy(tmp, path, len + 1);
    for (size_t i = 1; i <= len; i++) {
        if (tmp[i] != '/' && tmp[i] != '\\' && tmp[i] != '\0') continue;
        char save = tmp[i];
        tmp[i] = '\0';
        MKDIR(tmp);
        tmp[i] = save;
    }
}

static void state_dir(const char *override_path, char *out, size_t outlen)
{
    const char *env;
    if (override_path && *override_path) {
        snprintf(out, outlen, "%s", override_path);
        return;
    }
    env = getenv("TUN_STATE_DIR");
    if (env && *env) {
        snprintf(out, outlen, "%s", env);
        return;
    }
    env = getenv("HOME");
    if (env && *env) {
        snprintf(out, outlen, "%s/.tun", env);
        return;
    }
#ifdef _WIN32
    env = getenv("PROGRAMDATA");
    if (env && *env) {
        snprintf(out, outlen, "%s/tun", env);
        return;
    }
    env = getenv("APPDATA");
    if (env && *env) {
        snprintf(out, outlen, "%s/tun", env);
        return;
    }
    snprintf(out, outlen, "tun");
#else
    env = getenv("USER");
    if (env && *env) {
        snprintf(out, outlen, "/tmp/tun-%s", env);
        return;
    }
    snprintf(out, outlen, "/tmp/tun");
#endif
}

static int load_or_make_device_id(const char *override_path, char *id, size_t idlen)
{
    char dir[CTRL_PORT_FILE_MAX * 2];
    char file[CTRL_PORT_FILE_MAX * 3];
    state_dir(override_path, dir, sizeof(dir));
    /* Create the whole path before splitting, so a nested state dir works and
     * the generated id below actually reaches disk instead of being lost. */
    mkdir_p(dir);
    /* Split the trailing filename off so the directory can be created. */
    char *slash = strrchr(dir, '/');
#ifdef _WIN32
    char *bslash = strrchr(dir, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    if (slash) {
        *slash = '\0';
        snprintf(file, sizeof(file), "%s/%s", dir, slash + 1);
    } else {
        snprintf(file, sizeof(file), "%s", dir);
    }
    if (strlen(file) + 12 >= sizeof(file)) return -1;
    strncat(file, "/device_id", sizeof(file) - strlen(file) - 1);

    FILE *f = fopen(file, "r");
    if (f) {
        char buf[64];
        if (fgets(buf, (int)sizeof(buf), f)) {
            size_t i = 0;
            while (buf[i] && i + 1 < idlen) {
                char c = buf[i];
                if (c == '\n' || c == '\r' || c == ' ' || c == '\t') break;
                id[i] = c;
                i++;
            }
            id[i] = '\0';
            fclose(f);
            if (i >= 8) return 0;
        } else {
            fclose(f);
        }
    }
    random_hex(id, ID_LEN / 2);
    f = fopen(file, "w");
    if (f) {
        fprintf(f, "%s\n", id);
        fclose(f);
    }
    return 0;
}

/* ---- Remembering the control server --------------------------------------- */
/*
 * The PSK is a group key, so a node that reboots would otherwise come back as
 * nothing at all and wait to be re-provisioned by hand. The server and the PSK
 * are therefore written beside the device id, and a boot hook re-runs us, so
 * the node rejoins on its own after a power cut.
 */

static int state_file(const char *override_path, const char *name,
                      char *out, size_t outlen)
{
    char dir[CTRL_PORT_FILE_MAX * 2];
    state_dir(override_path, dir, sizeof(dir));
    mkdir_p(dir);
    char *slash = strrchr(dir, '/');
#ifdef _WIN32
    char *bslash = strrchr(dir, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    if (slash) {
        *slash = '\0';
        if (snprintf(out, outlen, "%s/%s/%s", dir, slash + 1, name) >= (int)outlen)
            return -1;
    } else {
        if (snprintf(out, outlen, "%s/%s", dir, name) >= (int)outlen)
            return -1;
    }
    return 0;
}

static int load_connection(const char *override_path, char *server, size_t sn,
                           char *psk, size_t pn)
{
    char file[CTRL_PORT_FILE_MAX * 3];
    if (state_file(override_path, "connection", file, sizeof(file)) != 0) return -1;
    FILE *f = fopen(file, "r");
    if (!f) return -1;
    char line[CTRL_PORT_FILE_MAX * 2];
    server[0] = '\0';
    psk[0] = '\0';
    while (fgets(line, (int)sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (!strncmp(line, "server=", 7)) {
            if (strlen(line + 7) < sn) memcpy(server, line + 7, strlen(line + 7) + 1);
        } else if (!strncmp(line, "psk=", 4)) {
            if (strlen(line + 4) < pn) memcpy(psk, line + 4, strlen(line + 4) + 1);
        }
    }
    fclose(f);
    return (server[0] && psk[0]) ? 0 : -1;
}

static int save_connection(const char *override_path, const char *server,
                           const char *psk)
{
    char file[CTRL_PORT_FILE_MAX * 3];
    if (state_file(override_path, "connection", file, sizeof(file)) != 0) return -1;
    FILE *f = fopen(file, "w");
    if (!f) return -1;
    fprintf(f, "server=%s\npsk=%s\n", server, psk);
    fclose(f);
#ifndef _WIN32
    /* Holds a shared secret: owner read/write only. */
    if (chmod(file, S_IRUSR | S_IWUSR) != 0) { /* best effort */ }
#endif
    return 0;
}

static int running_as_root(void)
{
#ifdef _WIN32
    return 1; /* the registry open below is the real privilege check */
#else
    return geteuid() == 0;
#endif
}

/* Registers us to start again after a reboot. Every backend is best effort and
 * quietly does nothing where it does not apply or is not permitted, so a client
 * that cannot install a hook still runs normally from the command line.
 * The state dir has to be passed along explicitly: the hook re-runs us with no
 * arguments, so it could not otherwise find a non-default one. */
static void install_autostart(const char *exe, const char *state, const char *server,
                              const char *psk)
{
    if (!running_as_root()) return;
#ifdef _WIN32
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    char cmd[PATH_MAX_LEN * 3 + CTRL_PORT_FILE_MAX * 2];
    snprintf(cmd, sizeof(cmd), "\"%s\" %s %s %s", exe, server, psk, state);
    RegSetValueExA(k, "tun", 0, REG_SZ, (const BYTE *)cmd, (DWORD)(strlen(cmd) + 1));
    RegCloseKey(k);
#elif defined(__ANDROID__)
    /* Magisk runs every script in /data/adb/service.d as root at boot. */
    if (access("/data/adb/service.d", F_OK) != 0) return;
    FILE *f = fopen("/data/adb/service.d/tun.sh", "w");
    if (!f) return;
    fprintf(f, "#!/system/bin/sh\n"
               "# written by tun; re-runs the client after a reboot\n"
               "sleep 20\n"
               "%s %s %s %s >/dev/null 2>&1 &\n", exe, server, psk, state);
    fclose(f);
    chmod("/data/adb/service.d/tun.sh",
          S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
#else
    /* Guarded on systemd so Android, BSD and containers never get a stray unit. */
    if (access("/run/systemd/system", F_OK) != 0) return;
    FILE *f = fopen("/etc/systemd/system/tun.service", "w");
    if (!f) return;
    fprintf(f,
            "[Unit]\n"
            "Description=tun client\n"
            "After=network-online.target\n"
            "Wants=network-online.target\n\n"
            "[Service]\n"
            "Type=simple\n"
            "Environment=TUN_FOREGROUND=1\n"
            "ExecStart=\"%s\" %s %s %s\n"
            "Restart=always\n"
            "RestartSec=5\n\n"
            "[Install]\n"
            "WantedBy=multi-user.target\n",
            exe, server, psk, state);
    fclose(f);
    if (system("systemctl daemon-reload >/dev/null 2>&1 && "
               "systemctl enable tun.service >/dev/null 2>&1") != 0) {
        remove("/etc/systemd/system/tun.service");
    }
#endif
}

/* Absolute path of this binary, so the boot hook re-execs the real file rather
 * than whatever happened to be on $PATH. */
static void resolve_exe_path(const char *argv0, char *out, size_t outlen)
{
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)outlen);
    if (n && n < outlen) { out[n] = '\0'; return; }
#endif
    snprintf(out, outlen, "%s", argv0 ? argv0 : "tun");
}

/* ---- Control loop --------------------------------------------------------- */

static thread_t frp_thr;
static int frp_running = 0;

static void stop_session(int *running, thread_t *t, volatile int *stop)
{
    if (!*running) return;
    *stop = 1;
    thread_join(*t);
    *stop = 0;
    *running = 0;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/* Reads a 4-byte IPv4 address out of a control payload and returns it in the
 * network byte order that sin_addr.s_addr stores. The server writes the raw
 * address bytes (Go's ip.To4()), so the conversion belongs here, in one place:
 * leaving it to each call site is how 127.0.0.1 turns into 1.0.0.127 and every
 * client ends up punching at a black hole. */
static uint32_t rd_ip(const uint8_t *p)
{
    return htonl(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                 ((uint32_t)p[2] << 8) | (uint32_t)p[3]);
}

static int frame_peer_id(const struct ControlPacket *pkt, char *out)
{
    int i;
    for (i = 0; i < ID_LEN; i++) {
        char c = pkt->node_id[i];
        int hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return 0;
    }
    memcpy(out, pkt->node_id, ID_LEN);
    out[ID_LEN] = '\0';
    return 1;
}

static void handle_command(struct ControlPacket *pkt, const char *server_ip)
{
    char id[ID_LEN + 1];

    if (pkt->command == CMD_FRP) {
        uint16_t local_port = rd16(pkt->payload);
        uint16_t remote_port = rd16(pkt->payload + 2);
        stop_session(&frp_running, &frp_thr, &g_frp_stop);
        struct frp_arg *fa = (struct frp_arg *)malloc(sizeof(*fa));
        if (!fa) return;
        memset(fa, 0, sizeof(*fa));
        strncpy(fa->server_ip, server_ip, sizeof(fa->server_ip) - 1);
        fa->local_port = local_port;
        fa->remote_port = remote_port;
        if (thread_create(&frp_thr, frp_session, fa) != 0) free(fa);
        else frp_running = 1;
    } else if (pkt->command == CMD_P2P_HUB) {
        /* The node-id field of this frame is our own id, which we must stamp
         * on everything we send to the hub. */
        if (frame_peer_id(pkt, id)) {
            memcpy(g_node_id, id, ID_LEN);
            g_node_id[ID_LEN] = '\0';
            hub_start(rd_ip(pkt->payload), rd16(pkt->payload + 4));
        }
    } else if (pkt->command == CMD_P2P) {
        /* Legacy single-peer form: replaces whatever mesh was running. */
        p2p_clear();
        id[0] = '\0';
        p2p_start("", rd_ip(pkt->payload), rd16(pkt->payload + 4));
    } else if (pkt->command == CMD_P2P_ADD) {
        if (frame_peer_id(pkt, id)) p2p_start(id, rd_ip(pkt->payload), rd16(pkt->payload + 4));
    } else if (pkt->command == CMD_P2P_DEL) {
        if (frame_peer_id(pkt, id)) p2p_stop(id);
    } else if (pkt->command == CMD_P2P_CLR) {
        p2p_clear();
    } else if (pkt->command == CMD_HUB_STOP) {
        hub_stop();
    } else if (pkt->command == CMD_TRP) {
        uint32_t target_ip = rd_ip(pkt->payload);
        uint16_t service_port = rd16(pkt->payload + 4);
        uint16_t public_port = rd16(pkt->payload + 6);
        g_trp_stop = 0;
        struct trp_arg *ta = (struct trp_arg *)malloc(sizeof(*ta));
        if (!ta) return;
        memset(ta, 0, sizeof(*ta));
        strncpy(ta->server_ip, server_ip, sizeof(ta->server_ip) - 1);
        ta->target_ip = target_ip;
        ta->service_port = service_port;
        ta->public_port = public_port;
        thread_t th;
        if (thread_create(&th, trp_session, ta) != 0) free(ta);
        else thread_detach(th);
    } else if (pkt->command == CMD_RESET) {
        stop_session(&frp_running, &frp_thr, &g_frp_stop);
        p2p_clear();
        hub_stop();
        g_trp_stop = 1;
    }
    (void)server_ip;
}

int main(int argc, char *argv[])
{
    char server_ip[PATH_MAX_LEN];
    char psk[PATH_MAX_LEN];
    char state[CTRL_PORT_FILE_MAX * 2];
    char device_id[ID_LEN + 1];
    char exe[PATH_MAX_LEN];

    server_ip[0] = '\0';
    psk[0] = '\0';
    resolve_exe_path(argc > 0 ? argv[0] : NULL, exe, sizeof(exe));
    /* Resolve the state directory once. The boot hook re-runs us with no
     * arguments, so from here on it is passed explicitly rather than being
     * re-derived from argv. */
    state_dir(argc > 3 ? argv[3] : NULL, state, sizeof(state));

    if (argc >= 3) {
        snprintf(server_ip, sizeof(server_ip), "%s", argv[1]);
        snprintf(psk, sizeof(psk), "%s", argv[2]);
        /* Remember this server, and make sure we come back after a reboot. */
        if (save_connection(state, server_ip, psk) == 0)
            install_autostart(exe, state, server_ip, psk);
    } else if (load_connection(state, server_ip, sizeof(server_ip),
                               psk, sizeof(psk)) != 0) {
        fprintf(stderr, "usage: %s <control-server-ip> <psk> [state-dir]\n", exe);
        return 1;
    }

    mutex_init(&g_p2p_mu);
    mutex_init(&g_ctrl_mu);
    /* Both gates must be open before the first frame is handled: a command can
     * arrive before this point on a fast server, and dropping it would leave
     * the client permanently without a rendezvous socket. */
    g_p2p_ready = 1;
    g_hub_ready = 1;
    if (load_or_make_device_id(state, device_id, sizeof(device_id)) != 0) {
        random_hex(device_id, ID_LEN / 2);
    }
    /* Seed the node id from the persisted one so anything we send before the
     * server hands our id back in a CMD_P2P_HUB frame is still attributable. */
    memcpy(g_node_id, device_id, ID_LEN);
    g_node_id[ID_LEN] = '\0';

#ifndef _WIN32
    {
        /* A boot hook supervises us directly, so it needs us in the foreground. */
        const char *fg = getenv("TUN_FOREGROUND");
        if (!(fg && *fg && strcmp(fg, "0") != 0)) daemonize();
    }
#else
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
#endif
    while (1) {
        sock_t control_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (control_fd == INVALID_SOCKET) { SLEEP_MS(5000); continue; }
        struct sockaddr_in srv;
        memset(&srv, 0, sizeof(srv));
        srv.sin_family = AF_INET;
        srv.sin_port = htons(CTRL_PORT);
        srv.sin_addr.s_addr = inet_addr(server_ip);
        if (connect(control_fd, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
            CLOSE_SOCK(control_fd);
            SLEEP_MS(5000);
            continue;
        }
        /* "<psk>\n<device_id>\n" — the id is what makes a shared PSK usable
         * with more than one device. */
        if (send(control_fd, psk, (int)strlen(psk), 0) <= 0 ||
            send(control_fd, "\n", 1, 0) <= 0 ||
            send(control_fd, device_id, (int)strlen(device_id), 0) <= 0 ||
            send(control_fd, "\n", 1, 0) <= 0) {
            CLOSE_SOCK(control_fd);
            SLEEP_MS(2000);
            continue;
        }

        mutex_lock(&g_ctrl_mu);
        g_ctrl_fd = control_fd;
        mutex_unlock(&g_ctrl_mu);

        thread_t reporter;
        int have_reporter = thread_create(&reporter, status_thread, NULL) == 0;

        /* A reconnect means every punch target we were told about is stale:
         * the server re-sends the mesh once it has our new endpoint. */
        p2p_clear();

        struct ControlPacket pkt;
        while (read_full(control_fd, &pkt, sizeof(pkt)) == 0) {
            /* 0xFE is a client -> server status marker; the server never
             * sends one, so treat it as a frame boundary and ignore it. */
            if (pkt.command == STATUS_BYTE) continue;
            handle_command(&pkt, server_ip);
        }

        mutex_lock(&g_ctrl_mu);
        g_ctrl_fd = INVALID_SOCKET;
        mutex_unlock(&g_ctrl_mu);
        if (have_reporter) thread_join(reporter);
        CLOSE_SOCK(control_fd);
        p2p_clear();
        SLEEP_MS(2000);
    }
    return 0;
}
