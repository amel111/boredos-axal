// AXAL: Multi-connection TCP socket implementation
// Supports up to MAX_TCP_SOCKETS concurrent connections across all processes.
//
// Copyright (c) 2023-2026 Chris (boreddevnl)
// AXAL modifications by azzuhry (amel111)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.

#include "tcp_socket.h"
#include "spinlock.h"

// lwIP headers
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"

extern void serial_write(const char *str);
extern void serial_write_num(uint32_t n);
extern void network_process_frames(void);
extern void k_delay(uint32_t ms);

static tcp_socket_t sockets[MAX_TCP_SOCKETS];
static spinlock_t socket_lock = SPINLOCK_INIT;
static int next_socket_id = 1;
static bool socket_initialized = false;

// --- lwIP callbacks (per-socket) ---

static err_t socket_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)tpcb; (void)err;
    tcp_socket_t *sock = (tcp_socket_t *)arg;
    if (!sock) { if (p) pbuf_free(p); return ERR_OK; }

    if (p == NULL) {
        sock->closed = 1;
        return ERR_OK;
    }

    if (sock->recv_queue == NULL) {
        sock->recv_queue = p;
    } else {
        pbuf_chain((struct pbuf *)sock->recv_queue, p);
    }
    return ERR_OK;
}

static void socket_err_cb(void *arg, err_t err) {
    (void)err;
    tcp_socket_t *sock = (tcp_socket_t *)arg;
    if (!sock) return;
    sock->pcb = NULL;
    sock->connect_error = 1;
    sock->closed = 1;
}

static err_t socket_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err) {
    (void)tpcb;
    tcp_socket_t *sock = (tcp_socket_t *)arg;
    if (!sock) return ERR_OK;

    if (err == ERR_OK) {
        sock->connect_done = 1;
    } else {
        sock->connect_error = 1;
    }
    return ERR_OK;
}

// --- Public API ---

void tcp_socket_init(void) {
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        sockets[i].in_use = false;
        sockets[i].pcb = NULL;
        sockets[i].recv_queue = NULL;
        sockets[i].socket_id = 0;
    }
    socket_initialized = true;
    serial_write("[TCP/AXAL] Socket table initialized (");
    serial_write_num(MAX_TCP_SOCKETS);
    serial_write(" max connections)\n");
}

int tcp_socket_create(uint32_t pid) {
    if (!socket_initialized) return -1;

    uint64_t flags = spinlock_acquire_irqsave(&socket_lock);

    // Find free slot
    int slot = -1;
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (!sockets[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        spinlock_release_irqrestore(&socket_lock, flags);
        return -1; // No free sockets
    }

    sockets[slot].in_use = true;
    sockets[slot].owner_pid = pid;
    sockets[slot].pcb = NULL;
    sockets[slot].recv_queue = NULL;
    sockets[slot].connect_done = 0;
    sockets[slot].connect_error = 0;
    sockets[slot].closed = 0;
    sockets[slot].socket_id = next_socket_id++;

    spinlock_release_irqrestore(&socket_lock, flags);
    return sockets[slot].socket_id;
}

tcp_socket_t *tcp_socket_get(int socket_id) {
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (sockets[i].in_use && sockets[i].socket_id == socket_id) {
            return &sockets[i];
        }
    }
    return NULL;
}

int tcp_socket_connect(int socket_id, uint32_t ip, uint16_t port) {
    uint64_t flags = spinlock_acquire_irqsave(&socket_lock);
    tcp_socket_t *sock = tcp_socket_get(socket_id);
    if (!sock) { spinlock_release_irqrestore(&socket_lock, flags); return -1; }

    // Create lwIP PCB
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) { spinlock_release_irqrestore(&socket_lock, flags); return -1; }

    sock->pcb = pcb;
    sock->connect_done = 0;
    sock->connect_error = 0;
    sock->closed = 0;

    // Set callbacks with socket as arg
    tcp_arg(pcb, sock);
    tcp_recv(pcb, socket_recv_cb);
    tcp_err(pcb, socket_err_cb);

    ip4_addr_t dest;
    dest.addr = ip; // Already in network byte order

    err_t err = tcp_connect(pcb, &dest, port, socket_connected_cb);
    spinlock_release_irqrestore(&socket_lock, flags);

    if (err != ERR_OK) return -1;

    // Wait for connection (with timeout)
    uint32_t start = sys_now();
    while (sys_now() - start < 15000) {
        network_process_frames();
        if (sock->connect_done) return 0;
        if (sock->connect_error) return -1;
        k_delay(10);
    }
    return -1; // Timeout
}

int tcp_socket_send(int socket_id, const void *data, size_t len) {
    uint64_t flags = spinlock_acquire_irqsave(&socket_lock);
    tcp_socket_t *sock = tcp_socket_get(socket_id);
    if (!sock || !sock->pcb) { spinlock_release_irqrestore(&socket_lock, flags); return -1; }

    struct tcp_pcb *pcb = (struct tcp_pcb *)sock->pcb;
    err_t err = tcp_write(pcb, data, len, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) { spinlock_release_irqrestore(&socket_lock, flags); return -1; }
    tcp_output(pcb);
    spinlock_release_irqrestore(&socket_lock, flags);
    return (int)len;
}

int tcp_socket_recv(int socket_id, void *buf, size_t max_len) {
    uint32_t start = sys_now();

    while (1) {
        uint64_t flags = spinlock_acquire_irqsave(&socket_lock);
        tcp_socket_t *sock = tcp_socket_get(socket_id);
        if (!sock) { spinlock_release_irqrestore(&socket_lock, flags); return -1; }

        if (sock->recv_queue) {
            struct pbuf *q = (struct pbuf *)sock->recv_queue;
            size_t to_copy = max_len;
            if (to_copy > q->tot_len) to_copy = q->tot_len;
            if (to_copy > 0xFFFF) to_copy = 0xFFFF;

            size_t copied = pbuf_copy_partial(q, buf, (u16_t)to_copy, 0);
            struct pbuf *remainder = pbuf_free_header(q, (u16_t)copied);
            if (sock->pcb) tcp_recved((struct tcp_pcb *)sock->pcb, (u16_t)copied);
            sock->recv_queue = remainder;
            spinlock_release_irqrestore(&socket_lock, flags);
            return (int)copied;
        }

        if (sock->closed) { spinlock_release_irqrestore(&socket_lock, flags); return 0; }
        if (sock->connect_error) { spinlock_release_irqrestore(&socket_lock, flags); return -1; }
        spinlock_release_irqrestore(&socket_lock, flags);

        if (sys_now() - start >= 30000) return 0; // 30s timeout
        network_process_frames();
        k_delay(10);
    }
}

int tcp_socket_recv_nb(int socket_id, void *buf, size_t max_len) {
    network_process_frames();

    uint64_t flags = spinlock_acquire_irqsave(&socket_lock);
    tcp_socket_t *sock = tcp_socket_get(socket_id);
    if (!sock) { spinlock_release_irqrestore(&socket_lock, flags); return -1; }

    if (!sock->recv_queue) {
        int ret = sock->closed ? -2 : 0;
        spinlock_release_irqrestore(&socket_lock, flags);
        return ret;
    }

    struct pbuf *q = (struct pbuf *)sock->recv_queue;
    size_t to_copy = max_len;
    if (to_copy > q->tot_len) to_copy = q->tot_len;
    if (to_copy > 0xFFFF) to_copy = 0xFFFF;

    size_t copied = pbuf_copy_partial(q, buf, (u16_t)to_copy, 0);
    struct pbuf *remainder = pbuf_free_header(q, (u16_t)copied);
    if (sock->pcb) tcp_recved((struct tcp_pcb *)sock->pcb, (u16_t)copied);
    sock->recv_queue = remainder;
    spinlock_release_irqrestore(&socket_lock, flags);
    return (int)copied;
}

int tcp_socket_close(int socket_id) {
    uint64_t flags = spinlock_acquire_irqsave(&socket_lock);
    tcp_socket_t *sock = tcp_socket_get(socket_id);
    if (!sock) { spinlock_release_irqrestore(&socket_lock, flags); return -1; }

    if (sock->recv_queue) {
        pbuf_free((struct pbuf *)sock->recv_queue);
        sock->recv_queue = NULL;
    }
    if (sock->pcb) {
        tcp_abort((struct tcp_pcb *)sock->pcb);
        sock->pcb = NULL;
    }

    sock->in_use = false;
    sock->closed = 0;
    sock->connect_done = 0;
    sock->connect_error = 0;

    spinlock_release_irqrestore(&socket_lock, flags);
    return 0;
}

void tcp_socket_cleanup_pid(uint32_t pid) {
    uint64_t flags = spinlock_acquire_irqsave(&socket_lock);
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (sockets[i].in_use && sockets[i].owner_pid == pid) {
            if (sockets[i].recv_queue) {
                pbuf_free((struct pbuf *)sockets[i].recv_queue);
                sockets[i].recv_queue = NULL;
            }
            if (sockets[i].pcb) {
                tcp_abort((struct tcp_pcb *)sockets[i].pcb);
                sockets[i].pcb = NULL;
            }
            sockets[i].in_use = false;
        }
    }
    spinlock_release_irqrestore(&socket_lock, flags);
}
