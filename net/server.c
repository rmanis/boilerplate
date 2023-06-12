#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_CLIENTS 128

struct client {
    int fd;
    int status;
    struct sockaddr_in sockaddr;
};

struct server {
    int running;
    // socket to listen for connections on
    int fd;
    // sockets for connected clients
    struct client clients[MAX_CLIENTS];
    // number of clients connected
    int numclients;

    // buffer
    char msg[BUFSIZ];
    // fill of the buffer
    long num_msg;
};

void setup_server(struct server *server, char *port);
void server_process_fds(struct server *server, int do_stdin);

int setup_select(fd_set *fdset, int servsock, struct client *clients, int numclients);
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

    int maxfd = setup_select(&readfds, server->fd, server->clients, server->numclients);
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

int setup_select(fd_set *fdset, int servsock, struct client *clients, int numclients) {
    int maxfd = servsock;
    int i;

    FD_ZERO(fdset);
    FD_SET(servsock, fdset);
    FD_SET(STDIN_FILENO, fdset);
    for (i = 0; i < numclients; i++) {
        int fd = clients[i].fd;
        FD_SET(fd, fdset);
        if (fd > maxfd) {
            maxfd = fd;
        }
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

void server_process_client(struct server *server, struct client *client, char *buf, long buflen) {
    printf("server received %ld bytes\n", buflen);
}

void server_client_recv(struct server *server, fd_set *readfds) {
    int i;
    int oldnumclients = server->numclients;
    struct client *tmpclient;
    for (i = 0; i < oldnumclients; i++) {
        tmpclient = server->clients + i;
        if (FD_ISSET(tmpclient->fd, readfds)) {
            server->num_msg = recv(tmpclient->fd, server->msg, sizeof(server->msg), 0);
            if (server->num_msg > 0) {
                server_process_client(server, tmpclient, server->msg, server->num_msg);
                // server_handle_client_msg(server, tmpclient, server->msg, server->num_msg);
                memset(server->msg, 0, sizeof(server->msg));
            } else if (server->num_msg == 0) {
                printf("  got %zu bytes, setting %d as dead\n", server->num_msg, i);
                tmpclient->status = 0;
            } else if (errno) {
                perror("recv");
                FD_ZERO(readfds);
            }
        }
    }
}

int server_remove_dead_clients(struct server *server) {
    int num_removed = 0;
    int i = 0;
    struct client *tmpclient;
    while (i < server->numclients) {
        tmpclient = server->clients + i;
        if (!tmpclient->status) {
            // Handle closed connections here.
            close(tmpclient->fd);
            printf("remove client %d\n", i);
            // | 0 | 1 | 2 | 3 | 4 | ...
            //                   ^
            server->numclients--;
            // move the last client into the position we just removed
            memcpy(tmpclient, server->clients + server->numclients, sizeof(struct client));
            num_removed++;
        } else {
            i++;
        }
    }
    return num_removed;
}

void server_accept(struct server *server, fd_set *readfds) {
    struct client *tmpclient;
    int clientsock;
    if (FD_ISSET(server->fd, readfds)) {
        tmpclient = server->clients + server->numclients;
        clientsock = accept_connection(server->fd, tmpclient);
        if (clientsock < 0) {
            perror("accept_connection");
            FD_ZERO(readfds);
        } else {
            printf("accepted\n");
            server->numclients++;
            tmpclient->fd = clientsock;
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