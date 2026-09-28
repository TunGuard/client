#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
typedef SOCKET sock_t;
#define CLOSE_SOCK(s) closesocket(s)
#define SLEEP_MS(ms) Sleep(ms)
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
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define CLOSE_SOCK(s) close(s)
#define SLEEP_MS(ms) usleep((ms) * 1000)
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
#endif

#define CMD_RESET 0x00
#define CMD_FRP 0x01
#define CMD_P2P 0x02
#define CMD_TRP 0x03
#define CTRL_PORT 7000
#define RELAY_BUF 8192
#define TUN_PATH_MAX 512
#define TUN_LINE_MAX 256

/* TRP pull-mode reverse proxy payload (8 bytes):
 *   [0..3] target IPv4 (raw), 0 = 127.0.0.1
 *   [4..5] service port (big-endian) — port of the local service
 *   [6..7] public port   (big-endian) — server port the client dials back
 */

struct ControlPacket {
    uint8_t command;
    uint8_t payload[8];
};

static volatile int g_frp_stop = 0;
static volatile int g_p2p_stop = 0;
static volatile int g_trp_stop = 0;

static char g_exe_path[TUN_PATH_MAX];
static char g_cfg_path[TUN_PATH_MAX];

/* Absolute path of this binary, so the boot hooks can re-exec it.
 * On Windows the real path comes from the loader; argv[0] may just be a name. */
static void resolve_exe_path(const char *argv0)
{
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, g_exe_path, (DWORD)sizeof(g_exe_path));
    if (n && n < sizeof(g_exe_path)) { g_exe_path[n] = '\0'; return; }
#endif
    snprintf(g_exe_path, sizeof(g_exe_path), "%s", argv0 ? argv0 : "tun");
}

/* Config lives next to the binary by default, so it stays writable on Android
 * (app files dir), on a USB stick, and in /opt. TUN_CONFIG overrides it. */
static void resolve_cfg_path(void)
{
    const char *env = getenv("TUN_CONFIG");
    if (env && *env) {
        snprintf(g_cfg_path, sizeof(g_cfg_path), "%s", env);
        return;
    }
    char *slash = strrchr(g_exe_path, '/');
#ifdef _WIN32
    char *bslash = strrchr(g_exe_path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    if (slash) {
        size_t dlen = (size_t)(slash - g_exe_path);
        if (dlen + sizeof("/tun.conf") <= sizeof(g_cfg_path)) {
            memcpy(g_cfg_path, g_exe_path, dlen);
            memcpy(g_cfg_path + dlen, "/tun.conf", sizeof("/tun.conf"));
        } else {
            g_cfg_path[0] = '\0';
        }
    } else {
        snprintf(g_cfg_path, sizeof(g_cfg_path), "%s", "tun.conf");
    }
}

static int load_config(char *server, size_t sn, char *psk, size_t pn)
{
    FILE *f = fopen(g_cfg_path, "r");
    if (!f) return -1;
    char line[TUN_LINE_MAX];
    server[0] = '\0';
    psk[0] = '\0';
    while (fgets(line, (int)sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (!strncmp(line, "server=", 7)) snprintf(server, sn, "%s", line + 7);
        else if (!strncmp(line, "psk=", 4)) snprintf(psk, pn, "%s", line + 4);
    }
    fclose(f);
    return (server[0] && psk[0]) ? 0 : -1;
}

static int save_config(const char *server, const char *psk)
{
    FILE *f = fopen(g_cfg_path, "w");
    if (!f) return -1;
    fprintf(f, "server=%s\npsk=%s\n", server, psk);
    fclose(f);
#ifndef _WIN32
    if (chmod(g_cfg_path, S_IRUSR | S_IWUSR) != 0) { /* best effort */ }
#endif
    return 0;
}

static int running_as_root(void)
{
#ifdef _WIN32
    return 1; /* the registry write itself is the privilege test */
#else
    return geteuid() == 0;
#endif
}

/* Re-launch us on boot. Every backend is best effort and silently no-ops when
 * it is not applicable or not permitted, so the client still runs normally. */
static int install_autostart(const char *server, const char *psk)
{
    if (!running_as_root()) return -1;
#ifdef _WIN32
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return -1;
    char cmd[TUN_PATH_MAX + TUN_LINE_MAX * 2];
    snprintf(cmd, sizeof(cmd), "\"%s\" %s %s", g_exe_path, server, psk);
    LONG r = RegSetValueExA(k, "tun", 0, REG_SZ, (const BYTE *)cmd,
                            (DWORD)(strlen(cmd) + 1));
    RegCloseKey(k);
    return r == ERROR_SUCCESS ? 0 : -1;
#elif defined(__ANDROID__)
    /* Magisk runs everything in /data/adb/service.d as root at boot. */
    if (access("/data/adb/service.d", F_OK) != 0) return -1;
    FILE *f = fopen("/data/adb/service.d/tun.sh", "w");
    if (!f) return -1;
    fprintf(f, "#!/system/bin/sh\n"
               "# written by tun; re-runs the client after reboot\n"
               "sleep 20\n"
               "%s %s %s >/dev/null 2>&1 &\n", g_exe_path, server, psk);
    fclose(f);
    return chmod("/data/adb/service.d/tun.sh",
                 S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
#else
    /* Guard on systemd so Android/BSD/containers never get a stray unit file. */
    if (access("/run/systemd/system", F_OK) != 0) return -1;
    FILE *f = fopen("/etc/systemd/system/tun.service", "w");
    if (!f) return -1;
    fprintf(f,
            "[Unit]\n"
            "Description=tun client\n"
            "After=network-online.target\n"
            "Wants=network-online.target\n\n"
            "[Service]\n"
            "Type=simple\n"
            "Environment=TUN_FOREGROUND=1\n"
            "ExecStart=\"%s\" %s %s\n"
            "Restart=always\n"
            "RestartSec=5\n\n"
            "[Install]\n"
            "WantedBy=multi-user.target\n",
            g_exe_path, server, psk);
    fclose(f);
    if (system("systemctl daemon-reload >/dev/null 2>&1 && "
               "systemctl enable tun.service >/dev/null 2>&1") != 0)
        return -1;
    return 0;
#endif
}

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

struct p2p_arg {
    uint32_t target_ip;
    uint16_t target_port;
};

THREAD_FN(p2p_session)
{
    struct p2p_arg *pa = (struct p2p_arg *)arg;
    struct sockaddr_in tgt;
    sock_t u = socket(AF_INET, SOCK_DGRAM, 0);
    if (u == INVALID_SOCKET) { free(pa); THREAD_EXIT; }
    memset(&tgt, 0, sizeof(tgt));
    tgt.sin_family = AF_INET;
    tgt.sin_port = htons(pa->target_port);
    tgt.sin_addr.s_addr = pa->target_ip;
    if (connect(u, (struct sockaddr *)&tgt, sizeof(tgt)) < 0) {
        CLOSE_SOCK(u);
        free(pa);
        THREAD_EXIT;
    }
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    unsigned char probe[4] = { 'T', 'U', 'N', 0x00 };
    while (!g_p2p_stop) {
        int n = 0;
        unsigned char buf[2048];
#ifndef _WIN32
        do {
            n = recv(u, buf, (int)sizeof(buf), 0);
        } while (n < 0 && errno == EINTR && !g_p2p_stop);
#else
        n = recv(u, buf, (int)sizeof(buf), 0);
#endif
        if (n > 0) send(u, buf, (int)n, 0);
        send(u, probe, (int)sizeof(probe), 0);
        probe[3]++;
        SLEEP_MS(1000);
    }
    CLOSE_SOCK(u);
    free(pa);
    THREAD_EXIT;
}

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

static thread_t frp_thr;
static thread_t p2p_thr;
static int frp_running = 0;
static int p2p_running = 0;

static void stop_session(int *running, thread_t *t, volatile int *stop)
{
    if (!*running) return;
    *stop = 1;
    thread_join(*t);
    *stop = 0;
    *running = 0;
}

int main(int argc, char *argv[])
{
    resolve_exe_path(argv[0]);
    resolve_cfg_path();

    char server_ip[TUN_LINE_MAX];
    char psk[TUN_LINE_MAX];
    server_ip[0] = '\0';
    psk[0] = '\0';
    if (argc >= 3) {
        snprintf(server_ip, sizeof(server_ip), "%s", argv[1]);
        snprintf(psk, sizeof(psk), "%s", argv[2]);
        if (save_config(server_ip, psk) == 0) install_autostart(server_ip, psk);
    } else if (load_config(server_ip, sizeof(server_ip), psk, sizeof(psk)) != 0) {
        fprintf(stderr, "usage: %s <control-server-ip> <psk>\n", g_exe_path);
        return 1;
    }
#ifndef _WIN32
    {
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
        if (send(control_fd, psk, (int)strlen(psk), 0) <= 0) {
            CLOSE_SOCK(control_fd);
            SLEEP_MS(2000);
            continue;
        }
        if (send(control_fd, "\n", 1, 0) <= 0) {
            CLOSE_SOCK(control_fd);
            SLEEP_MS(2000);
            continue;
        }
        struct ControlPacket pkt;
        while (read_full(control_fd, &pkt, sizeof(pkt)) >= 0) {
            if (pkt.command == CMD_FRP) {
                uint16_t local_port = (uint16_t)((pkt.payload[0] << 8) | pkt.payload[1]);
                uint16_t remote_port = (uint16_t)((pkt.payload[2] << 8) | pkt.payload[3]);
                stop_session(&frp_running, &frp_thr, &g_frp_stop);
                struct frp_arg *fa = (struct frp_arg *)malloc(sizeof(*fa));
                if (!fa) continue;
                memset(fa, 0, sizeof(*fa));
                strncpy(fa->server_ip, server_ip, sizeof(fa->server_ip) - 1);
                fa->local_port = local_port;
                fa->remote_port = remote_port;
                if (thread_create(&frp_thr, frp_session, fa) != 0) free(fa);
                else frp_running = 1;
            } else if (pkt.command == CMD_P2P) {
                uint32_t target_ip;
                memcpy(&target_ip, pkt.payload, 4);
                uint16_t target_port = (uint16_t)((pkt.payload[4] << 8) | pkt.payload[5]);
                stop_session(&p2p_running, &p2p_thr, &g_p2p_stop);
                struct p2p_arg *pa = (struct p2p_arg *)malloc(sizeof(*pa));
                if (!pa) continue;
                pa->target_ip = target_ip;
                pa->target_port = target_port;
                if (thread_create(&p2p_thr, p2p_session, pa) != 0) free(pa);
                else p2p_running = 1;
            } else if (pkt.command == CMD_TRP) {
                uint32_t target_ip;
                memcpy(&target_ip, pkt.payload, 4);
                uint16_t service_port = (uint16_t)((pkt.payload[4] << 8) | pkt.payload[5]);
                uint16_t public_port = (uint16_t)((pkt.payload[6] << 8) | pkt.payload[7]);
                g_trp_stop = 0;
                struct trp_arg *ta = (struct trp_arg *)malloc(sizeof(*ta));
                if (!ta) continue;
                memset(ta, 0, sizeof(*ta));
                strncpy(ta->server_ip, server_ip, sizeof(ta->server_ip) - 1);
                ta->target_ip = target_ip;
                ta->service_port = service_port;
                ta->public_port = public_port;
                thread_t th;
                if (thread_create(&th, trp_session, ta) != 0) free(ta);
                else thread_detach(th);
            } else if (pkt.command == CMD_RESET) {
                stop_session(&frp_running, &frp_thr, &g_frp_stop);
                stop_session(&p2p_running, &p2p_thr, &g_p2p_stop);
                g_trp_stop = 1;
            }
        }
        CLOSE_SOCK(control_fd);
        SLEEP_MS(2000);
    }
    return 0;
}