/* Link test for the direct peer socket.
 *
 * Builds a real p2p_thread against a fake peer that does nothing but echo
 * datagrams back, which is what the far end of a real link does. Verifies that
 * a link which answers gets a measured round trip, and that one which stops
 * answering stops claiming to be verified.
 *
 * Built and run by `make test`. It includes tun.c directly because everything
 * worth testing here is static: the alternative is exporting internals purely
 * for the test. */
#define main tun_main
#include "../tun.c"
#undef main

static sock_t g_echo;
static struct sockaddr_in g_echo_addr;
static volatile int g_echo_running = 1;

/* A peer with no protocol of its own: whatever arrives goes straight back. */
THREAD_FN(echo_thread)
{
    unsigned char buf[2048];
    (void)arg;
    set_io_timeout(g_echo, SO_RCVTIMEO, 200);
    while (g_echo_running) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = (int)recvfrom(g_echo, buf, (int)sizeof(buf), 0,
                              (struct sockaddr *)&from, &flen);
        if (n > 0)
            sendto(g_echo, buf, n, 0, (struct sockaddr *)&from, sizeof(from));
    }
    THREAD_EXIT;
}

int main(void)
{
    socklen_t elen = sizeof(g_echo_addr);
    struct sockaddr_in local;
    sock_t warmup;
    thread_t echo;

    mutex_init(&g_p2p_mu);
    mutex_init(&g_ctrl_mu);
    g_p2p_ready = 1;
    g_hub_ready = 1;
    memcpy(g_node_id, "aaaa1111", ID_LEN);
    g_node_id[ID_LEN] = '\0';

    g_echo = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_echo == INVALID_SOCKET) return 1;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(0x7F000001);
    local.sin_port = 0;
    if (bind(g_echo, (struct sockaddr *)&local, sizeof(local)) < 0) return 1;
    if (getsockname(g_echo, (struct sockaddr *)&g_echo_addr, &elen) < 0) return 1;

    /* A punch probe from an unconnected socket, so the socket p2p_thread opens
     * finds its mapping already established and the test starts from a working
     * link rather than from a NAT that may never answer. */
    warmup = socket(AF_INET, SOCK_DGRAM, 0);
    {
        unsigned char hello[3 + ID_LEN];
        memcpy(hello, HUB_PROBE_MAGIC, 3);
        memcpy(hello + 3, g_node_id, ID_LEN);
        sendto(warmup, hello, (int)sizeof(hello), 0,
               (struct sockaddr *)&g_echo_addr, sizeof(g_echo_addr));
    }

    if (thread_create(&echo, echo_thread, NULL) != 0) return 1;
    p2p_start("bbbb2222", inet_addr("127.0.0.1"),
              (uint16_t)ntohs(g_echo_addr.sin_port));

    for (int i = 0; i < 100; i++) {
        SLEEP_MS(100);
        if (g_peers[0] && g_peers[0]->direct && g_peers[0]->tested) break;
    }
    if (!g_peers[0] || !g_peers[0]->direct) {
        fprintf(stderr, "FAIL: the link never went direct\n");
        return 1;
    }
    if (!g_peers[0]->tested) {
        fprintf(stderr, "FAIL: the link test never completed\n");
        return 1;
    }
    if (g_peers[0]->rtt_bucket == RTT_UNKNOWN_BUCKET) {
        fprintf(stderr, "FAIL: tested, but no round trip was recorded\n");
        return 1;
    }
    printf("ok - link answered, round trip in bucket %u (<= %ums)\n",
           g_peers[0]->rtt_bucket, g_rtt_bucket_ms[g_peers[0]->rtt_bucket]);

    /* Now go quiet. The result belongs to the path it was measured on, so a
     * peer that stops answering must stop being reported as verified. */
    g_echo_running = 0;
    for (int i = 0; i < 100; i++) {
        SLEEP_MS(100);
        if (g_peers[0] && !g_peers[0]->tested) break;
    }
    if (g_peers[0]->tested) {
        fprintf(stderr, "FAIL: a silent peer is still reported as verified\n");
        return 1;
    }
    printf("ok - a silent peer stops being reported as verified\n");

    p2p_stop("bbbb2222");
    thread_join(echo);
    CLOSE_SOCK(g_echo);
    CLOSE_SOCK(warmup);
    return 0;
}