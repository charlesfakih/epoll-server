#include <asm-generic/socket.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#define BUF_SIZE 4096
#define PORT "5050"
#define MAX_EVENTS 10

struct Message {
    uint64_t seq;
    uint64_t timestamp_ns;
};

uint64_t now() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}


int main() {
    constexpr int N = 10;
    int fd[N], epollfd, nfds;
    char buf[16];
    struct addrinfo hints;
    struct addrinfo *results;
    

    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = 0;
    hints.ai_protocol = 0;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;

    int s = getaddrinfo("localhost", PORT, &hints, &results);
    if (s != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
        return 1;
    }

    epollfd = epoll_create1(0);
    if (epollfd == -1) {
        perror("epoll_create1");
        return 1;
    }
    struct epoll_event ev, events[MAX_EVENTS];

    for (int i = 0; i < N; i++) {
        fd[i] = socket(results->ai_family, results->ai_socktype, results->ai_protocol);
        if (fd[i] == -1) { perror("socket"); continue; }
        if (connect(fd[i], results->ai_addr, results->ai_addrlen) != 0){
            perror("connect");
            return 1;
        }
        int flags = fcntl(fd[i], F_GETFL, 0);
        if (flags == -1) {
            perror("fcntl F_GETFL");
            close(fd[i]);
            return 1;
        }
        if (fcntl(fd[i], F_SETFL, flags | O_NONBLOCK) == -1) {
            perror("fcntl F_SETFL");
            close(fd[i]);
            return 1;
        }
        if (i == 0) continue;
        ev.events = EPOLLIN;
        ev.data.fd = fd[i];
        if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd[i], &ev) == -1) {
            perror("epoll_ctl ADD");
            close(fd[i]);
            return 1;
        }

    }
    freeaddrinfo(results);
    uint64_t counter = 0;
    for (;;) {
        Message message{counter++, now()};
        send(fd[0], &message, sizeof(Message), 0);
        nfds = epoll_wait(epollfd, events, MAX_EVENTS, 0);
        if (nfds == -1) {
            perror("epoll_wait");
            return 1;
        }
        
        for (int n = 0; n < nfds; ++n) {
            while (1) {
                ssize_t val = recv(events[n].data.fd, buf, sizeof(Message), 0);
                if (val == (ssize_t)sizeof(Message)) {
                    Message msg;
                    std::memcpy(&msg, buf, sizeof(Message));
                    uint64_t latency = now() - msg.timestamp_ns;
                    //printf("seq=%lu latency=%lu ns\n", msg.seq, latency);
                } else if (val == 0) {
                    //printf("Client closed the connection\n");
                    epoll_ctl(epollfd, EPOLL_CTL_DEL, events[n].data.fd, NULL);
                    close(events[n].data.fd);
                    break;
                } else if (errno == EAGAIN) {
                    //printf("Buffer drained\n");
                    break;
                } else {
                    //printf("REAL ERROR\n");
                    epoll_ctl(epollfd, EPOLL_CTL_DEL, events[n].data.fd, NULL);
                    close(events[n].data.fd);
                    break;
                }
            }
        }
        
    }
    close(epollfd);
    for (int i = 0; i < N; i++) close(fd[i]);
}