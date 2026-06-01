#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <netdb.h>
#include <cstring>

#define BUF_SIZE 4096
#define PORT "5050"
#define LISTEN_BACKLOG 10

int main() {
    int fd;
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
    hints.ai_canonname = NULL;
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

    while (1) {
        peer_addr_size = sizeof(peer_addr);
        int cfd = accept(fd, (struct sockaddr *) &peer_addr, &peer_addr_size);
        if (cfd == -1) {
            perror("accept");
            continue;
        }

        ssize_t n;
        while ((n = recv(cfd, buf, sizeof(buf) - 1, 0)) > 0) {
            buf[n] = '\0';
            printf("%s", buf);
        }
        close(cfd);
    }

    close(fd);
}