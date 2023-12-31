// Server program that stores its clients in a list, each with its
// own input buffer
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "util/list.h"

#define MAX_CLIENTS 128

struct client {
    int fd;
    int status;
    struct sockaddr_in sockaddr;
    char *buf;
    unsigned buf_size;
    unsigned buf_fill;
};

struct server {
    int running;
    // socket to listen for connections on
    int fd;
    // sockets for connected clients
    struct list *clients;

    // buffer
    char msg[BUFSIZ];
    // fill of the buffer
    long num_msg;
};

void setup_server(struct server *server, char *port);
void server_process_fds(struct server *server, int do_stdin);

int setup_select(fd_set *fdset, int servsock, struct list *clients);
void server_console(struct server *server, fd_set *readfds);
void server_client_recv(struct server *server, fd_set *readfds);
int server_remove_dead_clients(struct server *server);
void server_accept(struct server *server, fd_set *readfds);
int accept_connection(int servsock, struct client *c);
ssize_t recv_all(int fd, void *buf, size_t num_bytes, int flags);


// Creates the socket for accepting new connections
void setup_server(struct server *server, char *port) {
    struct addrinfo hints = { 0 };
    struct addrinfo *res = NULL;
    int yes = 1;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (!getaddrinfo(NULL, port, &hints, &res)) {
        server->fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    } else {
        perror("server_setup getaddrinfo");
        server->running = 0;
    }
    if (server->fd < 0) {
        perror("setup_servsock socket");
        server->running = 0;
    } else {
        if (setsockopt(server->fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes))) {
            perror("server_setup setsockopt REUSEADDR");
            server->running = 0;
        } else {
            printf("bind %s\n", port);
            if (bind(server->fd, res->ai_addr, res->ai_addrlen)) {
                perror("setup_servsock bind");
                server->running = 0;
            } else {
                printf("listen\n");
                if (listen(server->fd, MAX_CLIENTS)) {
                    perror("listen");
                } else {
                    server->running = 1;
                }
            }
        }
    }
    freeaddrinfo(res);
}

// Checks the connected sockets and optionally stdin.
// Call this in a loop
void server_process_fds(struct server *server, int do_stdin) {
    if (!server->running) return;

    fd_set readfds;
    int ready;
    struct timeval tv = {0};

    int maxfd = setup_select(&readfds, server->fd, server->clients);
    tv.tv_sec = 0;
    tv.tv_usec = 50000;
    ready = select(maxfd + 1, &readfds, NULL, NULL, &tv);
    if (ready > 0) {
        if (do_stdin) {
            server_console(server, &readfds);
        }
        server_client_recv(server, &readfds);
        server_remove_dead_clients(server);
        server_accept(server, &readfds);
    } else if (ready < 0) {
        perror("server_process_fds select");
    }
}

int setup_select(fd_set *fdset, int servsock, struct list *clients) {
    int maxfd = servsock;
    int i;

    FD_ZERO(fdset);
    FD_SET(servsock, fdset);
    FD_SET(STDIN_FILENO, fdset);
    while (clients) {
        struct client *c = clients->car;
        int fd = c->fd;
        FD_SET(fd, fdset);
        if (fd > maxfd) {
            maxfd = fd;
        }
        clients = clients->next;
    }
    return maxfd;
}

void server_console(struct server *server, fd_set *readfds) {
    ssize_t bytes_read;
    if (FD_ISSET(STDIN_FILENO, readfds)) {
        bytes_read = read(STDIN_FILENO, server->msg, sizeof(server->msg));
        if (bytes_read == 0) {
            // Control-D pressed
            server->running = 0;
        } else if (bytes_read < 0 && errno) {
            perror("server_console read");
            FD_ZERO(readfds);
        } else {
            // server->msg now contains input from the console
        }
    }
}

void server_process_client(struct server *server, struct client *client, char *data, long datalen) {
    printf("server received %ld bytes\n", datalen);
    for (unsigned i = 0; i < datalen; i++) {
        if (data[i] == '\n') {
            int oldcount = data - client->buf;
            int cmdlen = oldcount + i;
            char *cmd = strndup(client->buf, cmdlen);
            // Process client command here.
            printf("Full command received (%s)\n", cmd);
            free(cmd);
            // Shift any remaining data to the beginning of the buffer and start over
            int remain = client->buf_fill - (cmdlen + 1);
            memmove(client->buf, data + i + 1, remain);
            client->buf_fill = remain;
            data = client->buf;
            datalen = client->buf_fill;
            i = 0;
        }
    }
}

void server_client_recv(struct server *server, fd_set *readfds) {
    int i;
    struct list *clients = server->clients;
    while (clients) {
        struct client *c = clients->car;
        if (FD_ISSET(c->fd, readfds)) {
            char *dst = c->buf + c->buf_fill;
            // TODO: expand the buffer if we fill it without receiving a newline
            size_t recvd = recv(c->fd, dst, c->buf_size - c->buf_fill, 0);
            if (recvd > 0) {
                char *newdata = c->buf + c->buf_fill;
                c->buf_fill += recvd;
                server_process_client(server, c, newdata, recvd);
            } else if (recvd == 0) {
                printf("  got %zu bytes, setting %d as dead\n", recvd, i);
                c->status = 0;
            } else if (errno) {
                perror("recv");
                FD_ZERO(readfds);
            }
        }
        clients = clients->next;
    }
}

int server_remove_dead_clients(struct server *server) {
    int num_removed = 0;
    int i = 0;
    struct client *tmpclient;
    struct list **holder = &server->clients;
    while (*holder) {
        struct list *cell = *holder;
        tmpclient = cell->car;
        if (!tmpclient->status) {
            // Handle close connection here.
            printf("remove client %d\n", i);
            close(tmpclient->fd);
            *holder = cell->next;
            free(tmpclient);
            free(cell);
            num_removed++;
        } else {
            holder = &cell->next;
        }
        i++;
    }
    return num_removed;
}

struct client *make_client() {
    struct client *c = calloc(1, sizeof(struct client));
    c->buf_size = BUFSIZ;
    c->buf = calloc(1, c->buf_size);
    return c;
}

void add_client_to_list(struct list **list, struct client *client) {
    *list = cons(client, *list);
}

void server_accept(struct server *server, fd_set *readfds) {
    struct client *tmpclient;
    int clientsock;
    if (FD_ISSET(server->fd, readfds)) {
        tmpclient = make_client();
        clientsock = accept_connection(server->fd, tmpclient);
        if (clientsock < 0) {
            free(tmpclient);
            perror("accept_connection");
            FD_ZERO(readfds);
        } else {
            printf("accepted\n");
            tmpclient->fd = clientsock;
            add_client_to_list(&server->clients, tmpclient);
            // server_greet(server, clientsock);
        }
    }
}

int accept_connection(int servsock, struct client *c) {
    int fd = 0;
    socklen_t socklen;
    fd = accept(servsock, (struct sockaddr *) &c->sockaddr, &socklen);
    if (fd > 0) {
        char address[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &c->sockaddr.sin_addr, address, sizeof(address));
        printf("accepted fd %d (%s)\n", fd, address);
        c->status = fd;
    }
    return fd;
}

// recvs a specific number of bytes and keeps trying until we get them all
ssize_t recv_all(int fd, void *buf, size_t num_bytes, int flags) {
    char *dst = buf;
    ssize_t r = 0;
    ssize_t bytes_recived = 0;
    while (r < num_bytes) {
        bytes_recived = recv(fd, dst + r, num_bytes - r, flags);
        if (bytes_recived <= 0) {
            r = bytes_recived;
            break;
        } else {
            r += bytes_recived;
        }
    }
    return r;
}

int main(int argc, char **argv) {
    struct server s = {0};
    char *port = "19567";
    if (argc > 1) {
        port = argv[1];
    }
    setup_server(&s, port);
    do {
        server_process_fds(&s, 1);
    } while (s.running);
    return 0;
}
