#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdio>
#include <netdb.h>
#include <cstring>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>

#define PORT "5050"
#define LISTEN_BACKLOG 10
#define MAX_EVENTS 10
#define BUF_SIZE 4096

int main() {
    int fd, nfds, conn_sock;
    char buf[BUF_SIZE];
    struct addrinfo hints;
    struct addrinfo *results, *rp;
    socklen_t peer_addr_size;
    struct sockaddr_storage peer_addr;

    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    hints.ai_protocol = 0;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;

    int s = getaddrinfo(NULL, PORT, &hints, &results);
    if (s != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
        return 1;
    }

    for (rp = results; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) continue;

        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;

        close(fd);
    }

    freeaddrinfo(results);
    if (rp == NULL) {
        fprintf(stderr, "Could not bind\n");
        return 1;
    }

    if (listen(fd, LISTEN_BACKLOG) == -1) {
        perror("listen");
        return 1;
    }

    struct epoll_event ev, events[MAX_EVENTS];

    int epollfd = epoll_create1(0);
    if (epollfd == -1) {
        perror("epoll_create1");
        return 1;
    }

    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        perror("epoll_ctl: listening socket");
        return 1;
    }

    for (;;) {
        nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            perror("epoll_wait");
            return 1;
        }

        for (int n = 0; n < nfds; ++n) {
            if (events[n].data.fd == fd) {
                peer_addr_size = sizeof(peer_addr);
                conn_sock = accept(fd, (struct sockaddr *) &peer_addr, &peer_addr_size);
                if (conn_sock == -1) {
                    perror("accept");
                    continue;
                }

                int flags = fcntl(conn_sock, F_GETFL, 0);
                if (flags == -1) {
                    perror("fcntl F_GETFL");
                    close(conn_sock);
                    continue;
                }
                if (fcntl(conn_sock, F_SETFL, flags | O_NONBLOCK) == -1) {
                    perror("fcntl F_SETFL");
                    close(conn_sock);
                    continue;
                }

                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = conn_sock;
                if (epoll_ctl(epollfd, EPOLL_CTL_ADD, conn_sock, &ev) == -1) {
                    perror("epoll_ctl: conn_sock");
                    close(conn_sock);
                    continue;
                }
            } else {
                while (1) {
                    ssize_t val = recv(events[n].data.fd, buf, sizeof(buf) - 1, 0);
                    if (val > 0) {
                        buf[val] = '\0';
                        printf("%s", buf);
                    } else if (val == 0) {
                        printf("Client closed the connection\n");
                        epoll_ctl(epollfd, EPOLL_CTL_DEL, events[n].data.fd, NULL);
                        close(events[n].data.fd);
                        break;
                    } else if (errno == EAGAIN) {
                        printf("Buffer drained\n");
                        break;
                    } else {
                        printf("REAL ERROR\n");
                        epoll_ctl(epollfd, EPOLL_CTL_DEL, events[n].data.fd, NULL);
                        close(events[n].data.fd);
                        break;
                    }
                }
            }
        }
    }

    close(epollfd);
    close(fd);
}