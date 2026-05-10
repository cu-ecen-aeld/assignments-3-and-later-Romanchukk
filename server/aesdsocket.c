#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <syslog.h>
#include <errno.h>

#include <string.h>
#include <time.h>

#include <signal.h>

#include <sys/queue.h>
#include <pthread.h>

#define APP_LOG_TAG     __FILE__
#define APP_LOG_LEVEL   (LOG_INFO | LOG_ERR | LOG_DEBUG)

#define APP_SOCKET_DEFAULT_PORT     (9000)
#define APP_SOCKET_BUFF_SIZE        (1024)

#define APP_OUTPUT_FILE             "/var/tmp/aesdsocketdata"
#define APP_OUTPUT_FILE_MODE        (0644)

typedef struct conn_node {
    bool is_completed;
    pthread_t tid;
    int connfd;
    char client_ip[INET_ADDRSTRLEN];
    SLIST_ENTRY(conn_node) entries;
} conn_node_t;

SLIST_HEAD(conn_thread_list, conn_node) head;

static volatile sig_atomic_t caught_signal = 0;

static pthread_mutex_t output_file_mtx;

static void *conn_handler(void *arg)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);

    conn_node_t *node = (conn_node_t *)arg;
    int connfd = node->connfd;

    char *rcvbuff = malloc(APP_SOCKET_BUFF_SIZE);
    if (!rcvbuff) {
        syslog(LOG_ERR, "conn_handler: failed to allocate rcvbuff");
        close(connfd);
        node->is_completed = true;
        pthread_exit(NULL);
    }

    char *packet = NULL;
    size_t packet_len = 0;
    int rcv_data_len;

    while ((rcv_data_len = recv(connfd, rcvbuff, APP_SOCKET_BUFF_SIZE, 0)) > 0) {
        char *new_packet = realloc(packet, packet_len + rcv_data_len);
        if (!new_packet) {
            syslog(LOG_ERR, "conn_handler: failed to realloc packet");
            free(packet);
            free(rcvbuff);
            close(connfd);
            node->is_completed = true;
            pthread_exit(NULL);
        }
        packet = new_packet;
        memcpy(packet + packet_len, rcvbuff, rcv_data_len);
        packet_len += rcv_data_len;

        char *newline;
        while ((newline = memchr(packet, '\n', packet_len)) != NULL) {
            size_t line_len = newline - packet + 1;

            pthread_mutex_lock(&output_file_mtx);

            int fd = open(APP_OUTPUT_FILE, O_WRONLY | O_CREAT | O_APPEND, APP_OUTPUT_FILE_MODE);
            if (fd == -1) {
                syslog(LOG_ERR, "conn_handler: open for write failed: %s (%d)", strerror(errno), errno);
                pthread_mutex_unlock(&output_file_mtx);
                free(packet);
                free(rcvbuff);
                close(connfd);
                node->is_completed = true;
                pthread_exit(NULL);
            }

            ssize_t wdata_len = write(fd, packet, line_len);
            close(fd);

            if (wdata_len != (ssize_t)line_len) {
                syslog(LOG_ERR, "conn_handler: short write");
                pthread_mutex_unlock(&output_file_mtx);
                free(packet);
                free(rcvbuff);
                close(connfd);
                node->is_completed = true;
                pthread_exit(NULL);
            }

            pthread_mutex_unlock(&output_file_mtx);

            size_t remaining = packet_len - line_len;
            memmove(packet, packet + line_len, remaining);
            packet_len = remaining;
            fd = open(APP_OUTPUT_FILE, O_RDONLY);
            if (fd == -1) {
                syslog(LOG_ERR, "conn_handler: open for read failed: %s (%d)", strerror(errno), errno);
                free(packet);
                free(rcvbuff);
                close(connfd);
                node->is_completed = true;
                pthread_exit(NULL);
            }

            int rdata_len;
            bool send_error = false;
            while ((rdata_len = read(fd, rcvbuff, APP_SOCKET_BUFF_SIZE)) > 0) {
                if (send(connfd, rcvbuff, rdata_len, 0) != rdata_len) {
                    syslog(LOG_ERR, "conn_handler: send failed: %s (%d)", strerror(errno), errno);
                    send_error = true;
                    break;
                }
            }
            close(fd);

            if (send_error) {
                free(packet);
                free(rcvbuff);
                close(connfd);
                node->is_completed = true;
                pthread_exit(NULL);
            }
        }
    }

    if (rcv_data_len == -1) {
        syslog(LOG_ERR, "conn_handler: recv() error: %s (%d)", strerror(errno), errno);
    }

    syslog(LOG_INFO, "Closed connection from %s", node->client_ip);
    printf("Closed connection from %s\n", node->client_ip);

    free(packet);
    free(rcvbuff);
    close(connfd);
    node->connfd = -1;
    node->is_completed = true;
    pthread_exit(NULL);
}

static void *timer_thread(void *arg)
{
    (void)arg;

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);

    while (!caught_signal) {
        for (int i = 0; i < 100 && !caught_signal; i++) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000L };
            nanosleep(&ts, NULL);
        }

        if (caught_signal) {
            break;
        }

        time_t now = time(NULL);
        struct tm tm_info;
        localtime_r(&now, &tm_info);

        char timestamp[128];
        strftime(timestamp, sizeof(timestamp), "timestamp:%a, %d %b %Y %T %z\n", &tm_info);

        pthread_mutex_lock(&output_file_mtx);

        int fd = open(APP_OUTPUT_FILE, O_WRONLY | O_CREAT | O_APPEND, APP_OUTPUT_FILE_MODE);
        if (fd != -1) {
            ssize_t w = write(fd, timestamp, strlen(timestamp));
            if (w < 0) {
                syslog(LOG_ERR, "timer_thread: write failed: %s (%d)", strerror(errno), errno);
            }
            close(fd);
        } else {
            syslog(LOG_ERR, "timer_thread: open failed: %s (%d)", strerror(errno), errno);
        }

        pthread_mutex_unlock(&output_file_mtx);
    }

    pthread_exit(NULL);
}

static void signal_handler(int signo)
{
    caught_signal = signo;
}

int main(int argc, char *argv[])
{
    int daemon_mode = 0;
    int opt;
    while ((opt = getopt(argc, argv, "d")) != -1) {
        if (opt == 'd') {
            daemon_mode = 1;
        } else {
            fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
            return -1;
        }
    }

    printf("Start: %s\n", APP_LOG_TAG);
    openlog(APP_LOG_TAG, LOG_PID, APP_LOG_LEVEL);
    syslog(LOG_INFO, "Running "__FILE__);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1) {
        syslog(LOG_ERR, "Failed to register signal handler");
        printf("Failed to register signal handler\n");
        closelog();
        return -1;
    }

    pthread_mutex_init(&output_file_mtx, NULL);
    SLIST_INIT(&head);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        syslog(LOG_ERR, "Failed to create socker: sockfd == %d", sockfd);
        printf("Failed to create socker: sockfd == %d\n", sockfd);
        closelog();
        return -1;
    }

    int reuse = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(APP_SOCKET_DEFAULT_PORT),
        .sin_addr = { .s_addr = INADDR_ANY }
    };

    int bindres = bind(sockfd, (struct sockaddr *) &addr, sizeof(addr));
    if (bindres != 0) {
        syslog(LOG_ERR, "Failed to bind socket: %s (%d)", strerror(errno), errno);
        printf("Failed to bind socket: %s (%d)\n", strerror(errno), errno);
        closelog();
        return -1;
    }

    if (daemon_mode) {
        pid_t pid = fork();
        if (pid == -1) {
            syslog(LOG_ERR, "Failed to fork: %s (%d)", strerror(errno), errno);
            printf("Failed to fork: %s (%d)\n", strerror(errno), errno);
            close(sockfd);
            closelog();
            return -1;
        }
        if (pid > 0) {
            close(sockfd);
            closelog();
            return EXIT_SUCCESS;
        }

        setsid();
        if (chdir("/") == -1) {
            syslog(LOG_ERR, "Failed to chdir: %s", strerror(errno));
        }
    }

    int listres = listen(sockfd, 5);
    if (listres == -1) {
        syslog(LOG_ERR, "Failed to listed socket: listres == -1");
        printf("Failed to listed socket: listres == -1\n");
        closelog();
        return -1;
    }


    pthread_t timer_tid;
    if (pthread_create(&timer_tid, NULL, timer_thread, NULL) != 0) {
        syslog(LOG_ERR, "Failed to create timer thread: %s (%d)", strerror(errno), errno);
        close(sockfd);
        pthread_mutex_destroy(&output_file_mtx);
        closelog();
        return -1;
    }

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_size = sizeof(client_addr);
        printf("Await client connection\n");

        int connfd = accept(
            sockfd,
            (struct sockaddr *) &client_addr,
            &client_addr_size
        );

        if (connfd < 0) {
            if (caught_signal) {
                break;
            }
            syslog(LOG_ERR, "Failed to recive client connection");
            printf("Failed to recive client connection\n");
            close(sockfd);
            pthread_join(timer_tid, NULL);
            pthread_mutex_destroy(&output_file_mtx);
            closelog();
            return -1;
        }

        conn_node_t *node = malloc(sizeof(conn_node_t));
        if (!node) {
            syslog(LOG_ERR, "Failed to allocate conn_node");
            close(connfd);
            continue;
        }
        node->is_completed = false;
        node->connfd = connfd;
        inet_ntop(AF_INET, &client_addr.sin_addr, node->client_ip, sizeof(node->client_ip));

        printf("Accepted connection from %s\n", node->client_ip);
        syslog(LOG_INFO, "Accepted connection from %s", node->client_ip);

        SLIST_INSERT_HEAD(&head, node, entries);

        if (pthread_create(&node->tid, NULL, conn_handler, node) != 0) {
            syslog(LOG_ERR, "Failed to create thread for %s", node->client_ip);
            close(connfd);
            SLIST_REMOVE(&head, node, conn_node, entries);
            free(node);
            continue;
        }

        conn_node_t *cur = SLIST_FIRST(&head);
        while (cur != NULL) {
            conn_node_t *next = SLIST_NEXT(cur, entries);
            if (cur->is_completed) {
                pthread_join(cur->tid, NULL);
                SLIST_REMOVE(&head, cur, conn_node, entries);
                free(cur);
            }
            cur = next;
        }
    }

    if (caught_signal) {
        syslog(LOG_INFO, "Caught signal, exiting");
        printf("Caught signal, exiting\n");
    }

    close(sockfd);

    conn_node_t *c;
    SLIST_FOREACH(c, &head, entries) {
        if (c->connfd != -1) {
            shutdown(c->connfd, SHUT_RDWR);
        }
    }

    conn_node_t *cur = SLIST_FIRST(&head);
    while (cur != NULL) {
        conn_node_t *next = SLIST_NEXT(cur, entries);
        pthread_join(cur->tid, NULL);
        SLIST_REMOVE(&head, cur, conn_node, entries);
        free(cur);
        cur = next;
    }

    pthread_join(timer_tid, NULL);

    pthread_mutex_destroy(&output_file_mtx);
    remove(APP_OUTPUT_FILE);
    closelog();
    return EXIT_SUCCESS;
}