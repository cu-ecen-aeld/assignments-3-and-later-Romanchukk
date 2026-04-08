#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <syslog.h>
#include <errno.h>

#include <string.h>

#include <signal.h>

#define APP_LOG_TAG     __FILE__
#define APP_LOG_LEVEL   (LOG_INFO | LOG_ERR | LOG_DEBUG)

#define APP_SOCKET_DEFAULT_PORT     (9000)
#define APP_SOCKET_BUFF_SIZE        (1024)

#define APP_OUTPUT_FILE             "/var/tmp/aesdsocketdata"
#define APP_OUTPUT_FILE_MODE        (0644)

static volatile sig_atomic_t caught_signal = 0;

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
    
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        syslog(LOG_ERR, "Failed to create socker: sockfd == %d", sockfd);
        printf("Failed to create socker: sockfd == %d\n", sockfd);
        closelog();
        return -1;
    }

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
        // close(STDIN_FILENO);
        // close(STDOUT_FILENO);
        // close(STDERR_FILENO);
        // open("/dev/null", O_RDONLY); // stdin
        // open("/dev/null", O_WRONLY); // stdout
        // open("/dev/null", O_WRONLY); // stderr
    }

    int listres = listen(sockfd, 5);
    if (listres == -1) {
        syslog(LOG_ERR, "Failed to listed socket: listres == -1");
        printf("Failed to listed socket: listres == -1\n");
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

            closelog();
            return -1;
        }

        char *client_ip = inet_ntoa(client_addr.sin_addr);
        printf("Accepted connection from %s\n", client_ip);
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        char *rcvbuff = malloc(sizeof(char) * APP_SOCKET_BUFF_SIZE);
        char *packet = NULL;
        size_t packet_len = 0;
        int rcv_data_len;

        while ((rcv_data_len = recv(connfd, rcvbuff, APP_SOCKET_BUFF_SIZE, 0)) > 0) {
            packet = realloc(packet, packet_len + rcv_data_len);
            if (packet == NULL) {
                printf("Failed to allocate memory for packet\n");
                syslog(LOG_ERR, "Failed to allocate memory for packet");
                free(rcvbuff);
                close(connfd);
                close(sockfd);
                closelog();
                return -1;
            }
            memcpy(packet + packet_len, rcvbuff, rcv_data_len);
            packet_len += rcv_data_len;

            char *newline;
            while ((newline = memchr(packet, '\n', packet_len)) != NULL) {
                size_t line_len = newline - packet + 1; // include \n

                printf("Save data: %.*s", (int)line_len, packet);

                int fd = open(APP_OUTPUT_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (fd == -1) {
                    printf("Failed to open file '%s': %s (%d)\n", APP_OUTPUT_FILE, strerror(errno), errno);
                    syslog(LOG_ERR, "Failed to open file '%s': %s (%d)", APP_OUTPUT_FILE, strerror(errno), errno);
                    free(packet);
                    free(rcvbuff);
                    close(connfd);
                    close(sockfd);
                    closelog();
                    return -1;
                }

                ssize_t wdata_len = write(fd, packet, line_len);
                if (wdata_len != (ssize_t)line_len) {
                    printf("Failed to write received data\n");
                    syslog(LOG_ERR, "Failed to write received data");
                    free(packet);
                    free(rcvbuff);
                    close(fd);
                    close(connfd);
                    close(sockfd);
                    closelog();
                    return -1;
                }
                close(fd);

                size_t remaining = packet_len - line_len;
                memmove(packet, packet + line_len, remaining);
                packet_len = remaining;

                fd = open(APP_OUTPUT_FILE, O_RDONLY);
                if (fd == -1) {
                    printf("Failed to open file for read '%s': %s (%d)\n", APP_OUTPUT_FILE, strerror(errno), errno);
                    syslog(LOG_ERR, "Failed to open file for read '%s': %s (%d)", APP_OUTPUT_FILE, strerror(errno), errno);
                    free(packet);
                    free(rcvbuff);
                    close(connfd);
                    close(sockfd);
                    closelog();
                    return -1;
                }

                int rdata_len;
                while ((rdata_len = read(fd, rcvbuff, APP_SOCKET_BUFF_SIZE)) > 0) {
                    int send_len = send(connfd, rcvbuff, rdata_len, 0);
                    if (send_len != rdata_len) {
                        printf("Failed to send file data '%s': %s (%d)\n", APP_OUTPUT_FILE, strerror(errno), errno);
                        syslog(LOG_ERR, "Failed to send file data '%s': %s (%d)", APP_OUTPUT_FILE, strerror(errno), errno);
                        close(fd);
                        free(packet);
                        free(rcvbuff);
                        close(connfd);
                        close(sockfd);
                        closelog();
                        return -1;
                    }
                }
                close(fd);
            }
        }

        if (rcv_data_len == -1) {
            syslog(LOG_ERR, "Failed to rcv data: recv() == -1");
            printf("Failed to rcv data: recv() == -1\n");
        }

        printf("Closed connection from %s\n", client_ip);
        syslog(LOG_INFO, "Closed connection from %s", client_ip);

        free(packet);
        free(rcvbuff);
        close(connfd);
    }

    if (caught_signal) {
        syslog(LOG_INFO, "Caught signal, exiting");
        printf("Caught signal, exiting\n");
    }

    close(sockfd);
    remove(APP_OUTPUT_FILE);
    closelog();
    return EXIT_SUCCESS;
}