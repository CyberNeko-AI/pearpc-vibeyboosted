/*
 * PearPC
 * slirpeth.cc
 *
 * User-mode NAT ethernet tunnel backend based on libslirp
 *
 * Copyright (C) 2026 PearPC contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>

#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <deque>
#include <set>
#include <string>
#include <vector>

#include "system/sysethtun.h"
#include "tools/except.h"
#include "slirpeth.h"

#ifdef HAVE_SLIRP
#include <libslirp.h>

/*
 * Virtual network layout (same defaults as QEMU's user networking):
 *
 *   10.0.2.0/24   virtual network
 *   10.0.2.2      host (gateway, DHCP server)
 *   10.0.2.3      virtual DNS server (proxied by libslirp)
 *   10.0.2.15     address handed to the guest via DHCP
 */
#define SLIRP_NETWORK     "10.0.2.0"
#define SLIRP_NETMASK     "255.255.255.0"
#define SLIRP_HOST_IP     "10.0.2.2"
#define SLIRP_DHCP_START  "10.0.2.15"
#define SLIRP_DNS_IP      "10.0.2.3"

#define SLIRP_MAX_GUEST_FRAMES    1024
#define SLIRP_MAX_PENDING_FRAMES  256

static bool slirpethDebug()
{
    static int v = -1;
    if (v < 0) v = getenv("SLIRPETH_DEBUG") ? 1 : 0;
    return v == 1;
}

struct SlirpTimer {
    bool active;
    int64_t expire_ns;
    void *cb_opaque;
    SlirpTimerId id;
};

struct SlirpTimerCmp {
    bool operator()(const SlirpTimer *a, const SlirpTimer *b) const
    {
        if (a->expire_ns != b->expire_ns) return a->expire_ns < b->expire_ns;
        return a < b;
    }
};

struct SlirpEthTunDevice::Impl {
    Slirp *slirp;

    // libslirp keeps POINTERS to these; they must outlive slirp_new()
    SlirpCb cb;
    SlirpConfig cfg;

    // frames produced by libslirp, destined for the guest NIC
    std::deque<std::vector<uint8_t>> txq;
    pthread_mutex_t txLock;
    pthread_cond_t txCond;

    // frames sent by the guest NIC, to be fed to libslirp
    std::deque<std::vector<uint8_t>> rxq;
    pthread_mutex_t rxLock;

    // worker thread wakeup
    int wakePipe[2];
    pthread_t worker;
    bool quit;
    bool running;

    // timers (only the IPv6 RA timer currently; kept general)
    std::set<SlirpTimer *> allTimers;
    std::set<SlirpTimer *, SlirpTimerCmp> activeTimers;

    // hostfwd rules registered before initDevice()
    std::vector<std::string> hostfwds;

    Impl()
    {
        slirp = NULL;
        memset(&txLock, 0, sizeof txLock);
        memset(&txCond, 0, sizeof txCond);
        memset(&rxLock, 0, sizeof rxLock);
        wakePipe[0] = wakePipe[1] = -1;
        quit = false;
        running = false;
    }
};

/*
 * libslirp callbacks. All of them receive the Impl pointer as opaque.
 */

static slirp_ssize_t cbSendPacket(const void *buf, size_t len, void *opaque);
static void cbGuestError(const char *msg, void *opaque);
static int64_t cbClockGetNs(void *opaque);
static void *cbTimerNew(SlirpTimerId id, void *cb_opaque, void *opaque);
static void cbTimerFree(void *timer, void *opaque);
static void cbTimerMod(void *timer, int64_t expire_time, void *opaque);
static void cbNotify(void *opaque);
static void cbInitCompleted(Slirp *slirp, void *opaque);
static void cbRegisterPollSocket(slirp_os_socket socket, void *opaque);
static void cbUnregisterPollSocket(slirp_os_socket socket, void *opaque);
static int cbAddPoll(int fd, int events, void *opaque);
static int cbGetREvents(int idx, void *opaque);

static int64_t cbClockGetNs(void *opaque)
{
    (void)opaque;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void cbGuestError(const char *msg, void *opaque)
{
    (void)opaque;
    fprintf(stderr, "[slirpeth] guest error: %s\n", msg);
}

static void cbInitCompleted(Slirp *slirp, void *opaque)
{
    SlirpEthTunDevice::Impl *d = (SlirpEthTunDevice::Impl *)opaque;
    d->slirp = slirp;
}

static void *cbTimerNew(SlirpTimerId id, void *cb_opaque, void *opaque)
{
    SlirpEthTunDevice::Impl *d = (SlirpEthTunDevice::Impl *)opaque;
    SlirpTimer *t = new SlirpTimer;
    t->id = id;
    t->cb_opaque = cb_opaque;
    t->active = false;
    t->expire_ns = 0;
    d->allTimers.insert(t);
    return t;
}

static void cbTimerFree(void *timer, void *opaque)
{
    SlirpEthTunDevice::Impl *d = (SlirpEthTunDevice::Impl *)opaque;
    SlirpTimer *t = (SlirpTimer *)timer;
    if (!t) return;
    if (t->active) {
        d->activeTimers.erase(t);
        t->active = false;
    }
    d->allTimers.erase(t);
    delete t;
}

static void cbTimerMod(void *timer, int64_t expire_time, void *opaque)
{
    SlirpEthTunDevice::Impl *d = (SlirpEthTunDevice::Impl *)opaque;
    SlirpTimer *t = (SlirpTimer *)timer;
    if (!t) return;
    if (t->active) {
        d->activeTimers.erase(t);
    }
    t->expire_ns = expire_time;
    t->active = true;
    d->activeTimers.insert(t);
}

static void wakeWorker(SlirpEthTunDevice::Impl *d)
{
    if (d->wakePipe[1] < 0) return;
    char c = 1;
    if (write(d->wakePipe[1], &c, 1) < 0) {
        /* pipe full or gone; the worker is about to wake up anyway */
    }
}

static void cbNotify(void *opaque)
{
    wakeWorker((SlirpEthTunDevice::Impl *)opaque);
}

static void cbRegisterPollSocket(slirp_os_socket socket, void *opaque)
{
    (void)socket;
    (void)opaque;
    /* the pollfd array is rebuilt on every iteration */
}

static void cbUnregisterPollSocket(slirp_os_socket socket, void *opaque)
{
    (void)socket;
    (void)opaque;
}

static int cbAddPoll(int fd, int events, void *opaque)
{
    std::vector<struct pollfd> *fds = (std::vector<struct pollfd> *)opaque;
    short pe = 0;
    if (events & SLIRP_POLL_IN) pe |= POLLIN;
    if (events & SLIRP_POLL_OUT) pe |= POLLOUT;
    if (events & SLIRP_POLL_PRI) pe |= POLLPRI;
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = pe;
    pfd.revents = 0;
    fds->push_back(pfd);
    if (slirpethDebug()) {
        fprintf(stderr, "[slirpeth] add_poll fd=%d events=%04x -> idx=%d\n", fd, events, (int)fds->size() - 1);
    }
    return (int)fds->size() - 1;
}

static int cbGetREvents(int idx, void *opaque)
{
    std::vector<struct pollfd> *fds = (std::vector<struct pollfd> *)opaque;
    if (idx < 0 || idx >= (int)fds->size()) return 0;
    short rev = (*fds)[idx].revents;
    int r = 0;
    if (rev & POLLIN) r |= SLIRP_POLL_IN;
    if (rev & POLLOUT) r |= SLIRP_POLL_OUT;
    if (rev & POLLPRI) r |= SLIRP_POLL_PRI;
    if (rev & POLLERR) r |= SLIRP_POLL_ERR;
    if (rev & POLLHUP) r |= SLIRP_POLL_HUP;
    if (slirpethDebug() && r) {
        fprintf(stderr, "[slirpeth] get_revents idx=%d revents=%04x -> %d\n", idx, (int)rev, r);
    }
    return r;
}

static slirp_ssize_t cbSendPacket(const void *buf, size_t len, void *opaque)
{
    SlirpEthTunDevice::Impl *d = (SlirpEthTunDevice::Impl *)opaque;
    if (!len || len > 65536) return -1;
    if (slirpethDebug()) {
        const uint8_t *p = (const uint8_t *)buf;
        uint16_t et = (uint16_t)((p[12] << 8) | p[13]);
        fprintf(stderr, "[slirpeth] tx to guest len=%zu ethertype=%04x\n", len, et);
    }
    pthread_mutex_lock(&d->txLock);
    if (d->txq.size() < SLIRP_MAX_GUEST_FRAMES) {
        const uint8_t *p = (const uint8_t *)buf;
        d->txq.emplace_back(p, p + len);
        pthread_cond_signal(&d->txCond);
        pthread_mutex_unlock(&d->txLock);
        return (slirp_ssize_t)len;
    }
    pthread_mutex_unlock(&d->txLock);
    /*
     * Queue full: drop the frame and pretend it was sent. The guest
     * simply won't acknowledge it, so libslirp's TCP will retransmit
     * at a lower pace.
     */
    return (slirp_ssize_t)len;
}

/*
 * The PacketDevice implementation
 */

SlirpEthTunDevice::SlirpEthTunDevice()
{
    d = new Impl;
    pthread_mutex_init(&d->txLock, NULL);
    pthread_cond_init(&d->txCond, NULL);
    pthread_mutex_init(&d->rxLock, NULL);
}

SlirpEthTunDevice::~SlirpEthTunDevice()
{
    shutdownDevice();
    pthread_mutex_destroy(&d->txLock);
    pthread_cond_destroy(&d->txCond);
    pthread_mutex_destroy(&d->rxLock);
    delete d;
}

uint SlirpEthTunDevice::recvPacket(void *buf, uint size)
{
    pthread_mutex_lock(&d->txLock);
    if (d->txq.empty()) {
        pthread_mutex_unlock(&d->txLock);
        return 0;
    }
    std::vector<uint8_t> f = std::move(d->txq.front());
    d->txq.pop_front();
    pthread_mutex_unlock(&d->txLock);
    uint n = (f.size() < size) ? (uint)f.size() : size;
    memcpy(buf, f.data(), n);
    return n;
}

int SlirpEthTunDevice::waitRecvPacket()
{
    pthread_mutex_lock(&d->txLock);
    while (d->txq.empty() && !d->quit) {
        pthread_cond_wait(&d->txCond, &d->txLock);
    }
    bool have = !d->txq.empty();
    pthread_mutex_unlock(&d->txLock);
    return have ? 0 : ENODEV;
}

uint SlirpEthTunDevice::sendPacket(void *buf, uint size)
{
    if (!size) return 0;
    bool queued = false;
    pthread_mutex_lock(&d->rxLock);
    if (!d->quit && d->rxq.size() < SLIRP_MAX_PENDING_FRAMES) {
        uint8_t *p = (uint8_t *)buf;
        d->rxq.emplace_back(p, p + size);
        queued = true;
    }
    pthread_mutex_unlock(&d->rxLock);
    if (queued) {
        wakeWorker(d);
    }
    return queued ? size : 0;
}

uint SlirpEthTunDevice::getWriteFramePrefix()
{
    return 0;
}

static bool parseHostForwardSpec(const char *spec, int &isUdp, int &hostPort, int &guestPort)
{
    const char *p = spec;
    if (strncmp(p, "udp:", 4) == 0) {
        isUdp = 1;
        p += 4;
    } else if (strncmp(p, "tcp:", 4) == 0) {
        isUdp = 0;
        p += 4;
    } else {
        return false;
    }
    hostPort = 0;
    guestPort = 0;
    if (sscanf(p, "%d:%d", &hostPort, &guestPort) != 2) return false;
    if (hostPort <= 0 || hostPort > 65535) return false;
    if (guestPort <= 0 || guestPort > 65535) return false;
    return true;
}

bool SlirpEthTunDevice::applyHostForward(const char *spec)
{
    int isUdp, hostPort, guestPort;
    if (!parseHostForwardSpec(spec, isUdp, hostPort, guestPort)) return false;
    struct in_addr hostAddr, guestAddr;
    hostAddr.s_addr = htonl(INADDR_ANY);
    if (inet_pton(AF_INET, SLIRP_DHCP_START, &guestAddr) != 1) return false;
    if (!d->slirp) return false;
    return slirp_add_hostfwd(d->slirp, isUdp, hostAddr, hostPort, guestAddr, guestPort) == 0;
}

bool SlirpEthTunDevice::addHostForward(const char *spec)
{
    if (!spec || !*spec) return false;
    int isUdp, hostPort, guestPort;
    if (!parseHostForwardSpec(spec, isUdp, hostPort, guestPort)) return false;
    if (d->running) {
        return applyHostForward(spec);
    }
    d->hostfwds.push_back(std::string(spec));
    return true;
}

static void drainGuestFrames(SlirpEthTunDevice::Impl *d)
{
    std::deque<std::vector<uint8_t>> frames;
    pthread_mutex_lock(&d->rxLock);
    frames.swap(d->rxq);
    pthread_mutex_unlock(&d->rxLock);
    while (!frames.empty()) {
        std::vector<uint8_t> &f = frames.front();
        if (slirpethDebug()) {
            uint16_t et = (uint16_t)((f[12] << 8) | f[13]);
            fprintf(stderr, "[slirpeth] input frame len=%d ethertype=%04x\n", (int)f.size(), et);
        }
        slirp_input(d->slirp, f.data(), (int)f.size());
        frames.pop_front();
    }
}

static void fireExpiredTimers(SlirpEthTunDevice::Impl *d, int64_t now)
{
    while (!d->activeTimers.empty()) {
        SlirpTimer *t = *d->activeTimers.begin();
        if (t->expire_ns > now) break;
        d->activeTimers.erase(d->activeTimers.begin());
        t->active = false;
        slirp_handle_timer(d->slirp, t->id, t->cb_opaque);
    }
}

void SlirpEthTunDevice::workerLoop()
{
    std::vector<struct pollfd> fds;
    fds.reserve(64);

    while (!d->quit) {
        fds.clear();
        struct pollfd wakePfd;
        wakePfd.fd = d->wakePipe[0];
        wakePfd.events = POLLIN;
        wakePfd.revents = 0;
        fds.push_back(wakePfd);

        uint32_t slirpTimeout = UINT32_MAX;
        slirp_pollfds_fill_socket(d->slirp, &slirpTimeout, cbAddPoll, &fds);

        int64_t now = cbClockGetNs(d);
        int tmo = (slirpTimeout == UINT32_MAX) ? -1 : (int)slirpTimeout;
        if (!d->activeTimers.empty()) {
            int64_t deltaMs = ((*d->activeTimers.begin())->expire_ns - now) / 1000000;
            if (deltaMs < 0) deltaMs = 0;
            if (tmo < 0 || deltaMs < tmo) tmo = (int)deltaMs;
        }
        if (tmo < 0 || tmo > 1000) tmo = 1000;

        int r = poll(fds.data(), (nfds_t)fds.size(), tmo);
        if (r < 0 && errno != EINTR) {
            fprintf(stderr, "[slirpeth] poll error: %s\n", strerror(errno));
        }

        now = cbClockGetNs(d);
        fireExpiredTimers(d, now);

        if (fds[0].revents & POLLIN) {
            char buf[256];
            while (read(d->wakePipe[0], buf, sizeof buf) > 0) {
            }
        }

        slirp_pollfds_poll(d->slirp, (r < 0) ? 1 : 0, cbGetREvents, &fds);

        drainGuestFrames(d);
    }
}

static void *workerMain(void *arg)
{
    SlirpEthTunDevice *dev = (SlirpEthTunDevice *)arg;
    dev->workerLoop();
    return NULL;
}

int SlirpEthTunDevice::initDevice()
{
    if (d->running) return 0;

    memset(&d->cfg, 0, sizeof d->cfg);
    d->cfg.version = SLIRP_CONFIG_VERSION_MAX;
    d->cfg.restricted = 0;
    d->cfg.in_enabled = 1;
    inet_pton(AF_INET, SLIRP_NETWORK, &d->cfg.vnetwork);
    inet_pton(AF_INET, SLIRP_NETMASK, &d->cfg.vnetmask);
    inet_pton(AF_INET, SLIRP_HOST_IP, &d->cfg.vhost);
    d->cfg.in6_enabled = false;
    inet_pton(AF_INET, SLIRP_DHCP_START, &d->cfg.vdhcp_start);
    inet_pton(AF_INET, SLIRP_DNS_IP, &d->cfg.vnameserver);
    d->cfg.if_mtu = 1500;
    d->cfg.if_mru = 0; /* default */
    d->cfg.disable_host_loopback = false;
    d->cfg.enable_emu = false;

    memset(&d->cb, 0, sizeof d->cb);
    d->cb.send_packet = cbSendPacket;
    d->cb.guest_error = cbGuestError;
    d->cb.clock_get_ns = cbClockGetNs;
    d->cb.timer_new_opaque = cbTimerNew;
    d->cb.timer_free = cbTimerFree;
    d->cb.timer_mod = cbTimerMod;
    d->cb.notify = cbNotify;
    d->cb.init_completed = cbInitCompleted;
    d->cb.register_poll_socket = cbRegisterPollSocket;
    d->cb.unregister_poll_socket = cbUnregisterPollSocket;

    d->slirp = slirp_new(&d->cfg, &d->cb, d);
    if (!d->slirp) {
        throw MsgException("libslirp failed to initialize");
    }
    if (slirpethDebug()) {
        slirp_set_debug(SLIRP_DBG_CALL | SLIRP_DBG_MISC | SLIRP_DBG_ERROR |
                        SLIRP_DBG_VERBOSE_CALL);
    }

    for (size_t i = 0; i < d->hostfwds.size(); i++) {
        if (!applyHostForward(d->hostfwds[i].c_str())) {
            fprintf(stderr, "[slirpeth] failed to add host forward '%s' (port busy?)\n",
                    d->hostfwds[i].c_str());
        } else {
            fprintf(stderr, "[slirpeth] host forward: %s\n", d->hostfwds[i].c_str());
        }
    }

    if (pipe(d->wakePipe) < 0) {
        slirp_cleanup(d->slirp);
        d->slirp = NULL;
        throw IOException(errno);
    }
    fcntl(d->wakePipe[0], F_SETFL, O_NONBLOCK);
    fcntl(d->wakePipe[1], F_SETFL, O_NONBLOCK);

    if (pthread_create(&d->worker, NULL, workerMain, this) != 0) {
        int e = errno;
        close(d->wakePipe[0]);
        close(d->wakePipe[1]);
        d->wakePipe[0] = d->wakePipe[1] = -1;
        slirp_cleanup(d->slirp);
        d->slirp = NULL;
        throw IOException(e);
    }
    d->running = true;
    printf("[slirpeth] user-mode NAT active: guest=%s/%s gateway=%s dns=%s\n",
           SLIRP_DHCP_START, SLIRP_NETMASK, SLIRP_HOST_IP, SLIRP_DNS_IP);
    return 0;
}

int SlirpEthTunDevice::shutdownDevice()
{
    if (!d->running) return 0;
    d->quit = true;
    wakeWorker(d);
    pthread_join(d->worker, NULL);

    slirp_cleanup(d->slirp);
    d->slirp = NULL;

    close(d->wakePipe[0]);
    close(d->wakePipe[1]);
    d->wakePipe[0] = d->wakePipe[1] = -1;

    while (!d->allTimers.empty()) {
        SlirpTimer *t = *d->allTimers.begin();
        d->activeTimers.erase(t);
        d->allTimers.erase(d->allTimers.begin());
        delete t;
    }

    pthread_mutex_lock(&d->txLock);
    d->txq.clear();
    pthread_cond_broadcast(&d->txCond);
    pthread_mutex_unlock(&d->txLock);
    d->running = false;
    return 0;
}

EthTunDevice *createSlirpEthernetTunnel()
{
    return new SlirpEthTunDevice();
}

#else /* !HAVE_SLIRP */

SlirpEthTunDevice::SlirpEthTunDevice()
{
    d = NULL;
}

SlirpEthTunDevice::~SlirpEthTunDevice()
{
}

uint SlirpEthTunDevice::recvPacket(void *buf, uint size)
{
    (void)buf;
    (void)size;
    return 0;
}

int SlirpEthTunDevice::waitRecvPacket()
{
    return ENODEV;
}

uint SlirpEthTunDevice::sendPacket(void *buf, uint size)
{
    (void)buf;
    (void)size;
    return 0;
}

uint SlirpEthTunDevice::getWriteFramePrefix()
{
    return 0;
}

int SlirpEthTunDevice::initDevice()
{
    throw MsgException("this build has no libslirp support "
                       "(install libslirp, e.g. 'brew install libslirp')");
}

int SlirpEthTunDevice::shutdownDevice()
{
    return 0;
}

bool SlirpEthTunDevice::addHostForward(const char *spec)
{
    (void)spec;
    return false;
}

EthTunDevice *createSlirpEthernetTunnel()
{
    throw MsgException("this build has no libslirp support "
                       "(install libslirp, e.g. 'brew install libslirp')");
}

#endif /* HAVE_SLIRP */
