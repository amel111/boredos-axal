// AXAL: Multi-connection TCP socket table
// Replaces the single global tcp_pcb with per-socket state.
// Each process can own multiple sockets identified by socket_fd.
//
// Copyright (c) 2023-2026 Chris (boreddevnl)
// AXAL modifications by azzuhry (amel111)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
#ifndef TCP_SOCKET_H
#define TCP_SOCKET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Maximum concurrent TCP connections system-wide
#define MAX_TCP_SOCKETS 32

typedef struct tcp_socket {
    bool in_use;
    uint32_t owner_pid;         // Process that owns this socket
    void *pcb;                  // struct tcp_pcb* (void* to avoid lwip header dep)
    void *recv_queue;           // struct pbuf* receive buffer chain
    int connect_done;
    int connect_error;
    int closed;
    int socket_id;              // Unique ID for this socket
} tcp_socket_t;

// Initialize the socket table
void tcp_socket_init(void);

// Allocate a new socket for the calling process. Returns socket_id or -1.
int tcp_socket_create(uint32_t pid);

// Connect a socket to a remote host. Returns 0 on success.
int tcp_socket_connect(int socket_id, uint32_t ip, uint16_t port);

// Send data on a socket. Returns bytes sent or -1.
int tcp_socket_send(int socket_id, const void *data, size_t len);

// Receive data from a socket (blocking). Returns bytes received, 0 on close, -1 on error.
int tcp_socket_recv(int socket_id, void *buf, size_t max_len);

// Receive data non-blocking. Returns bytes, 0 if nothing, -2 if closed, -1 on error.
int tcp_socket_recv_nb(int socket_id, void *buf, size_t max_len);

// Close and free a socket.
int tcp_socket_close(int socket_id);

// Close all sockets owned by a process (called on process exit).
void tcp_socket_cleanup_pid(uint32_t pid);

// Get socket by ID (NULL if invalid)
tcp_socket_t *tcp_socket_get(int socket_id);

#endif // TCP_SOCKET_H
