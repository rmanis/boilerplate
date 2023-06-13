//
// Server program that accepts a connection and runs a process in a
// pseudo terminal, transferring input and output between the socket
// and the pty.
//
// The objective was to be able to run a curses program over a telnet
// connection. See the notes file "curses-over-telnet".
//
// Work in progress
//
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <util.h>
#include "util/list.h"

#define MAX_CLIENTS 128

struct client {
    int fd;
    int status;
    pid_t pid;
    int master;
    int slave;
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
        fd = c->master;
        if (fd > maxfd) {
            maxfd = fd;
        }
        FD_SET(fd, fdset);
        clients = clients->next;
    }
    return maxfd;
}

void kill_children(struct server *server) {
    struct list *clients = server->clients;
    while (clients) {
        struct client *c = clients->car;
        if (c->pid) {
            kill(c->pid, SIGKILL);
        }
        close(c->master);
        clients = clients->next;
    }
}

void server_console(struct server *server, fd_set *readfds) {
    ssize_t bytes_read;
    if (FD_ISSET(STDIN_FILENO, readfds)) {
        bytes_read = read(STDIN_FILENO, server->msg, sizeof(server->msg));
        if (bytes_read == 0) {
            // Control-D pressed
            server->running = 0;
            kill_children(server);
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
            size_t recvd = recv(c->fd, dst, c->buf_size - c->buf_fill, 0);
            if (recvd > 0) {
                printf("%d received %lu (", i, recvd);
                fwrite(dst, recvd, 1, stdout);
                printf(")\n");
                size_t written = write(c->master, dst, recvd);
            } else if (recvd == 0) {
                printf("  got %zu bytes, setting %d as dead\n", recvd, i);
                c->status = 0;
            } else if (errno) {
                perror("recv");
                FD_ZERO(readfds);
            }
        }
        if (c->status && FD_ISSET(c->master, readfds)) {
            size_t nread = read(c->master, c->buf, c->buf_size);
            if (nread > 0) {
                // printf("%d read (%.*s)\n", i, (int) nread, c->buf);
                size_t sent = send(c->fd, c->buf, nread, 0);
            } else if (nread == 0) {
                printf("  read %zu bytes, setting %d as dead\n", nread, i);
                c->status = 0;
            } else if (errno) {
                perror("read");
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
            if (tmpclient->pid) {
                kill(tmpclient->pid, SIGKILL);
            }
            printf("remove client %d\n", i);
            close(tmpclient->fd);
            close(tmpclient->master);
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

void fork_client(struct server *server, struct client *client) {
    int stat = openpty(&client->master, &client->slave, NULL, NULL, NULL);
    client->pid = fork();
    if (client->pid == 0) {
        // child
        dup2(client->slave, STDIN_FILENO);
        dup2(client->slave, STDOUT_FILENO);
        dup2(client->slave, STDERR_FILENO);

        struct termios attrs = {0};
        tcgetattr(STDIN_FILENO, &attrs);
        attrs.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, 0, &attrs);

        close(client->master);
        close(server->fd);
        // execlp("./ech", "ech", 0);
        execlp("top", "top", 0);
    } else {
        close(client->slave);
        printf("child created %d\n", client->pid);
    }
}

void server_accept(struct server *server, fd_set *readfds) {
    struct client *client;
    int clientsock;
    if (FD_ISSET(server->fd, readfds)) {
        client = make_client();
        clientsock = accept_connection(server->fd, client);
        if (clientsock < 0) {
            free(client);
            perror("accept_connection");
            FD_ZERO(readfds);
        } else {
            printf("accepted\n");
            client->fd = clientsock;
            add_client_to_list(&server->clients, client);
            fork_client(server, client);
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
