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
#define CTRL_PORT 7000
#define RELAY_BUF 8192

struct ControlPacket {
    uint8_t command;
    uint8_t payload[8];
};

static volatile int g_frp_stop = 0;
static volatile int g_p2p_stop = 0;

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
    if (argc < 3) return 1;
    char *server_ip = argv[1];
    char *psk = argv[2];
#ifndef _WIN32
    daemonize();
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
            } else if (pkt.command == CMD_RESET) {
                stop_session(&frp_running, &frp_thr, &g_frp_stop);
                stop_session(&p2p_running, &p2p_thr, &g_p2p_stop);
            }
        }
        CLOSE_SOCK(control_fd);
        SLEEP_MS(2000);
    }
    return 0;
}