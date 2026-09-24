#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>

#define CMD_RESET 0x00
#define CMD_FRP 0x01
#define CMD_P2P 0x02
#define CTRL_PORT 7000
#define RELAY_BUF 8192

struct ControlPacket {
    uint8_t command;
    uint8_t payload[8];
};

void daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);
    if (setsid() < 0) exit(EXIT_FAILURE);
    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);
    umask(0);
    chdir("/");
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}

static int read_full(int fd, void *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = recv(fd, (char *)buf + off, len - off, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static void pump(int a, int b)
{
    fd_set fds;
    char buf[RELAY_BUF];
    while (1) {
        FD_ZERO(&fds);
        FD_SET(a, &fds);
        FD_SET(b, &fds);
        int m = a > b ? a : b;
        if (select(m + 1, &fds, NULL, NULL, NULL) < 0) break;
        if (FD_ISSET(a, &fds)) {
            ssize_t n = recv(a, buf, sizeof(buf), 0);
            if (n <= 0) break;
            if (send(b, buf, (size_t)n, 0) <= 0) break;
        }
        if (FD_ISSET(b, &fds)) {
            ssize_t n = recv(b, buf, sizeof(buf), 0);
            if (n <= 0) break;
            if (send(a, buf, (size_t)n, 0) <= 0) break;
        }
    }
}

static void frp_session(const char *server_ip, uint16_t local_port, uint16_t remote_port)
{
    int lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0) return;
    int one = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in lsa;
    memset(&lsa, 0, sizeof(lsa));
    lsa.sin_family = AF_INET;
    lsa.sin_addr.s_addr = htonl(INADDR_ANY);
    lsa.sin_port = htons(local_port);
    if (bind(lsock, (struct sockaddr *)&lsa, sizeof(lsa)) < 0) { close(lsock); return; }
    if (listen(lsock, 16) < 0) { close(lsock); return; }
    while (1) {
        int c = accept(lsock, NULL, NULL);
        if (c < 0) break;
        pid_t pid = fork();
        if (pid == 0) {
            close(lsock);
            int rs = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in rsa;
            memset(&rsa, 0, sizeof(rsa));
            rsa.sin_family = AF_INET;
            rsa.sin_port = htons(remote_port);
            rsa.sin_addr.s_addr = inet_addr(server_ip);
            if (connect(rs, (struct sockaddr *)&rsa, sizeof(rsa)) < 0) {
                close(c);
                close(rs);
                _exit(0);
            }
            pump(c, rs);
            close(c);
            close(rs);
            _exit(0);
        }
        close(c);
    }
    close(lsock);
}

static void p2p_session(uint32_t target_ip, uint16_t target_port)
{
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    if (u < 0) return;
    struct sockaddr_in tgt;
    memset(&tgt, 0, sizeof(tgt));
    tgt.sin_family = AF_INET;
    tgt.sin_port = htons(target_port);
    tgt.sin_addr.s_addr = target_ip;
    if (connect(u, (struct sockaddr *)&tgt, sizeof(tgt)) < 0) { close(u); return; }
    struct timeval tv = { 5, 0 };
    setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    unsigned char probe[4] = { 'T', 'U', 'N', 0x00 };
    while (1) {
        ssize_t n = 0;
        unsigned char buf[2048];
        do {
            n = recv(u, buf, sizeof(buf), 0);
        } while (n < 0 && errno == EINTR);
        if (n > 0) send(u, buf, (size_t)n, 0);
        send(u, probe, sizeof(probe), 0);
        probe[3]++;
        sleep(1);
    }
    close(u);
}

int main(int argc, char *argv[])
{
    if (argc < 3) return 1;
    char *server_ip = argv[1];
    char *psk = argv[2];
    daemonize();
    signal(SIGCHLD, SIG_IGN);
    pid_t frp_pid = 0, p2p_pid = 0;
    while (1) {
        int control_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (control_fd < 0) { sleep(5); continue; }
        struct sockaddr_in srv;
        memset(&srv, 0, sizeof(srv));
        srv.sin_family = AF_INET;
        srv.sin_port = htons(CTRL_PORT);
        srv.sin_addr.s_addr = inet_addr(server_ip);
        if (connect(control_fd, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
            close(control_fd);
            sleep(5);
            continue;
        }
        if (send(control_fd, psk, strlen(psk), 0) <= 0) {
            close(control_fd);
            sleep(2);
            continue;
        }
        struct ControlPacket pkt;
        while (read_full(control_fd, &pkt, sizeof(pkt)) >= 0) {
            if (pkt.command == CMD_FRP) {
                uint16_t local_port = (uint16_t)((pkt.payload[0] << 8) | pkt.payload[1]);
                uint16_t remote_port = (uint16_t)((pkt.payload[2] << 8) | pkt.payload[3]);
                if (frp_pid > 0) { kill(frp_pid, SIGKILL); frp_pid = 0; }
                frp_pid = fork();
                if (frp_pid == 0) { frp_session(server_ip, local_port, remote_port); _exit(0); }
            } else if (pkt.command == CMD_P2P) {
                uint32_t target_ip;
                memcpy(&target_ip, pkt.payload, 4);
                uint16_t target_port = (uint16_t)((pkt.payload[4] << 8) | pkt.payload[5]);
                if (p2p_pid > 0) { kill(p2p_pid, SIGKILL); p2p_pid = 0; }
                p2p_pid = fork();
                if (p2p_pid == 0) { p2p_session(target_ip, target_port); _exit(0); }
            } else if (pkt.command == CMD_RESET) {
                if (frp_pid > 0) { kill(frp_pid, SIGKILL); frp_pid = 0; }
                if (p2p_pid > 0) { kill(p2p_pid, SIGKILL); p2p_pid = 0; }
            }
        }
        close(control_fd);
        sleep(2);
    }
    return 0;
}