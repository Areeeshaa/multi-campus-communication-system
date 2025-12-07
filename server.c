#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#define TCP_PORT 9000
#define UDP_PORT 9001
#define MAXLINE 2048
#define MAX_CLIENTS 50
#define HEARTBEAT_TIMEOUT 30 // seconds, for display only

typedef struct {
    char campus[64];
    int tcp_fd;
    struct sockaddr_in udp_addr; // last-known UDP addr for that campus (from heartbeat)
    time_t last_seen; // last UDP heartbeat time
    int authenticated;
} campus_client_t;

campus_client_t clients[MAX_CLIENTS];
pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;

// Example hard-coded credentials
typedef struct {
    char campus[64];
    char pass[128];
} cred_t;

cred_t allowed_creds[] = {
    {"Lahore","NU-LHR-123"},
    {"Karachi","NU-KHI-123"},
    {"Islamabad","NU-ISB-123"},
    {"Peshawar","NU-PSR-123"},
    {"Multan","NU-MTN-123"},
    {"CFD","NU-CFD-123"},
    {"",""} // sentinel
};

void log_time_prefix() {
    time_t t = time(NULL);
    char s[64];
    struct tm tm;
    localtime_r(&t,&tm);
    strftime(s,sizeof(s),"%Y-%m-%d %H:%M:%S", &tm);
    printf("[%s] ", s);
}

int validate_credential(const char *campus, const char *pass) {
    for (int i=0; allowed_creds[i].campus[0]; ++i) {
        if (strcmp(allowed_creds[i].campus, campus) == 0 &&
            strcmp(allowed_creds[i].pass, pass) == 0) return 1;
    }
    return 0;
}

int find_client_index_by_campus(const char *campus) {
    for (int i=0;i<MAX_CLIENTS;i++) {
        if (clients[i].authenticated && strcmp(clients[i].campus, campus)==0) return i;
    }
    return -1;
}

int add_client(int fd) {
    pthread_mutex_lock(&clients_lock);
    int idx=-1;
    for (int i=0;i<MAX_CLIENTS;i++) {
        if (!clients[i].authenticated && clients[i].tcp_fd==0) { idx=i; break; }
    }
    if (idx==-1) {
        // try to find an entry with tcp_fd 0 as free slot
        for (int i=0;i<MAX_CLIENTS;i++) {
            if (clients[i].tcp_fd==0) { idx=i; break; }
        }
    }
    if (idx==-1) idx = -1;
    else {
        clients[idx].tcp_fd = fd;
        clients[idx].authenticated = 0;
        clients[idx].campus[0] = '\0';
        clients[idx].last_seen = 0;
        memset(&clients[idx].udp_addr, 0, sizeof(clients[idx].udp_addr));
    }
    pthread_mutex_unlock(&clients_lock);
    return idx;
}

void remove_client_index(int idx) {
    pthread_mutex_lock(&clients_lock);
    if (idx>=0 && idx<MAX_CLIENTS) {
        if (clients[idx].tcp_fd) close(clients[idx].tcp_fd);
        clients[idx].tcp_fd = 0;
        clients[idx].authenticated = 0;
        clients[idx].campus[0] = '\0';
        clients[idx].last_seen = 0;
        memset(&clients[idx].udp_addr, 0, sizeof(clients[idx].udp_addr));
    }
    pthread_mutex_unlock(&clients_lock);
}

ssize_t send_all(int fd, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t sent = send(fd, buf + total, len - total, 0);
        if (sent <= 0) return -1;
        total += sent;
    }
    return total;
}

void forward_message_to_campus(const char *to_campus, const char *from_campus, const char *from_dept, const char *body) {
    int idx = find_client_index_by_campus(to_campus);
    if (idx == -1) {
        log_time_prefix(); printf("Routing failed: target campus %s not connected.\n", to_campus);
        return;
    }
    char buf[MAXLINE];
    snprintf(buf, sizeof(buf), "FROM|FromCampus:%s|FromDept:%s|Body:%s\n", from_campus, from_dept, body);
    if (send_all(clients[idx].tcp_fd, buf, strlen(buf)) < 0) {
        log_time_prefix(); printf("Failed to send to %s (fd %d). Removing.\n", clients[idx].campus, clients[idx].tcp_fd);
        remove_client_index(idx);
    } else {
        log_time_prefix(); printf("Routed message from %s to %s (dept=%s): %s\n", from_campus, to_campus, from_dept, body);
    }
}

// Trim newline
void trim_newline(char *s) {
    char *p = strchr(s,'\r');
    if (p) *p = 0;
    p = strchr(s,'\n');
    if (p) *p = 0;
}

void *client_thread(void *arg) {
    int idx = *((int*)arg);
    free(arg);
    int fd;
    pthread_mutex_lock(&clients_lock);
    fd = clients[idx].tcp_fd;
    pthread_mutex_unlock(&clients_lock);
    char buf[MAXLINE];
    ssize_t n;
    char inbuf[MAXLINE];
    size_t inpos = 0;

    while (1) {
        n = recv(fd, buf, sizeof(buf)-1, 0);
        if (n <= 0) {
            log_time_prefix(); printf("TCP connection closed for index %d (fd %d).\n", idx, fd);
            remove_client_index(idx);
            break;
        }
        // accumulate and process line by line
        for (ssize_t i=0;i<n;i++) {
            inbuf[inpos++] = buf[i];
            if (inpos >= sizeof(inbuf)-1) inpos = sizeof(inbuf)-2;
            if (buf[i]=='\n') {
                inbuf[inpos]=0;
                trim_newline(inbuf);
                // parse
                if (!clients[idx].authenticated) {
                    // Expect AUTH|Campus:Name|Pass:pwd
                    if (strncmp(inbuf,"AUTH|",5)==0) {
                        char campus[64]="", pass[128]="";
                        char *p = strdup(inbuf+5);
                        char *tok = strtok(p, "|");
                        while (tok) {
                            if (strncmp(tok,"Campus:",7)==0) strncpy(campus, tok+7, sizeof(campus)-1);
                            if (strncmp(tok,"Pass:",5)==0) strncpy(pass, tok+5, sizeof(pass)-1);
                            tok = strtok(NULL,"|");
                        }
                        free(p);
                        if (validate_credential(campus, pass)) {
                            pthread_mutex_lock(&clients_lock);
                            strncpy(clients[idx].campus, campus, sizeof(clients[idx].campus)-1);
                            clients[idx].authenticated = 1;
                            pthread_mutex_unlock(&clients_lock);
                            send_all(fd, "AUTH_OK\n", 8);
                            log_time_prefix(); printf("Client authenticated: %s (fd %d, idx %d)\n", campus, fd, idx);
                        } else {
                            send_all(fd, "AUTH_FAIL\n", 10);
                            log_time_prefix(); printf("Authentication failed from fd %d. Closing.\n", fd);
                            remove_client_index(idx);
                            close(fd);
                            return NULL;
                        }
                    } else {
                        send_all(fd, "AUTH_REQUIRED\n", 14);
                        remove_client_index(idx);
                        close(fd);
                        return NULL;
                    }
                } else {
                    // Authenticated; expect MSG|...
                    if (strncmp(inbuf,"MSG|",4)==0) {
                        char toCampus[64]="", toDept[64]="", fromDept[64]="", body[MAXLINE]="";
                        char *p = strdup(inbuf+4);
                        char *tok = strtok(p,"|");
                        while (tok) {
                            if (strncmp(tok,"ToCampus:",9)==0) strncpy(toCampus, tok+9, sizeof(toCampus)-1);
                            if (strncmp(tok,"ToDept:",7)==0) strncpy(toDept, tok+7, sizeof(toDept)-1);
                            if (strncmp(tok,"FromDept:",9)==0) strncpy(fromDept, tok+9, sizeof(fromDept)-1);
                            if (strncmp(tok,"Body:",5)==0) strncpy(body, tok+5, sizeof(body)-1);
                            tok = strtok(NULL,"|");
                        }
                        free(p);
                        // find fromCampus name
                        char fromCampus[64];
                        pthread_mutex_lock(&clients_lock);
                        strncpy(fromCampus, clients[idx].campus, sizeof(fromCampus)-1);
                        pthread_mutex_unlock(&clients_lock);
                        forward_message_to_campus(toCampus, fromCampus, fromDept, body);
                    } else if (strncmp(inbuf,"ADMINCMD|",9)==0) {
                        // server may receive admin commands from admin console to send to some specific client over TCP
                        // we'll ignore from client
                        log_time_prefix(); printf("Received ADMINCMD from client (ignored).\n");
                    } else {
                        log_time_prefix(); printf("Unknown TCP message from %s: %s\n", clients[idx].campus, inbuf);
                    }
                }

                inpos = 0;
            }
        }
    }
    return NULL;
}

void *tcp_accept_thread(void *arg) {
    int listenfd = *((int*)arg);
    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int connfd = accept(listenfd, (struct sockaddr*)&cliaddr, &clilen);
        if (connfd < 0) {
            perror("accept");
            continue;
        }
        log_time_prefix(); printf("New TCP connection fd %d from %s:%d\n", connfd, inet_ntoa(cliaddr.sin_addr), ntohs(cliaddr.sin_port));
        int idx = add_client(connfd);
        if (idx == -1) {
            log_time_prefix(); printf("Max clients reached. Closing connection fd %d\n", connfd);
            close(connfd);
            continue;
        }
        int *ptr = malloc(sizeof(int));
        *ptr = idx;
        pthread_t tid;
        pthread_create(&tid, NULL, client_thread, ptr);
        pthread_detach(tid);
    }
    return NULL;
}

void *udp_listener_thread(void *arg) {
    int udpfd = *((int*)arg);
    char buf[MAXLINE];
    struct sockaddr_in cliaddr;
    socklen_t len = sizeof(cliaddr);
    while (1) {
        ssize_t n = recvfrom(udpfd, buf, sizeof(buf)-1, 0, (struct sockaddr*)&cliaddr, &len);
        if (n < 0) {
            perror("recvfrom");
            continue;
        }
        buf[n]=0;
        // expect HEARTBEAT|Campus:Name|UDPPort:port
        if (strncmp(buf,"HEARTBEAT|",10)==0) {
            char campus[64]="";
            int udpport = 0;
            char *p = strdup(buf+10);
            char *tok = strtok(p,"|");
            while (tok) {
                if (strncmp(tok,"Campus:",7)==0) strncpy(campus, tok+7, sizeof(campus)-1);
                if (strncmp(tok,"UDPPort:",8)==0) udpport = atoi(tok+8);
                tok = strtok(NULL,"|");
            }
            free(p);
            if (campus[0]) {
                pthread_mutex_lock(&clients_lock);
                int idx = find_client_index_by_campus(campus);
                if (idx == -1) {
                    // If client not known through TCP yet, we still may store info in a new slot
                    // try to find a free slot
                    for (int i=0;i<MAX_CLIENTS;i++) {
                        if (clients[i].tcp_fd==0 && !clients[i].authenticated) {
                            // store UDP info but no TCP
                            strncpy(clients[i].campus, campus, sizeof(clients[i].campus)-1);
                            clients[i].tcp_fd = 0;
                            clients[i].authenticated = 0;
                            clients[i].udp_addr = cliaddr;
                            clients[i].udp_addr.sin_port = htons(udpport);
                            clients[i].last_seen = time(NULL);
                            idx = i;
                            break;
                        }
                    }
                } else {
                    clients[idx].udp_addr = cliaddr;
                    clients[idx].udp_addr.sin_port = htons(udpport);
                    clients[idx].last_seen = time(NULL);
                }
                pthread_mutex_unlock(&clients_lock);
                log_time_prefix(); printf("Received HEARTBEAT from %s (udp %s:%d)\n",
                    campus, inet_ntoa(cliaddr.sin_addr), udpport);
            } else {
                log_time_prefix(); printf("Malformed HEARTBEAT: %s\n", buf);
            }
        } else {
            log_time_prefix(); printf("Unknown UDP packet: %s\n", buf);
        }
    }
    return NULL;
}

void admin_console_loop(int udpfd) {
    char line[1024];
    while (1) {
        printf("\n--- ADMIN ---\n");
        printf("Commands: list | broadcast <message> | sendcmd <Campus> <text> | quit\n> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        trim_newline(line);
        if (strncmp(line,"list",4)==0) {
            pthread_mutex_lock(&clients_lock);
            log_time_prefix(); printf("Connected campuses (TCP and UDP known):\n");
            for (int i=0;i<MAX_CLIENTS;i++) {
                if (clients[i].tcp_fd || clients[i].last_seen) {
                    char addrstr[64] = "unknown";
                    int port = 0;
                    if (clients[i].udp_addr.sin_family == AF_INET) {
                        inet_ntop(AF_INET, &clients[i].udp_addr.sin_addr, addrstr, sizeof(addrstr));
                        port = ntohs(clients[i].udp_addr.sin_port);
                    }
                    double secs = clients[i].last_seen ? difftime(time(NULL), clients[i].last_seen) : -1;
                    printf(" slot %2d | campus: %-10s | tcpfd: %3d | udp: %s:%d | last_seen: %s%s\n",
                        i, clients[i].campus[0]?clients[i].campus:"(none)", clients[i].tcp_fd, addrstr, port,
                        clients[i].last_seen ? ctime(&clients[i].last_seen) : "never",
                        secs>=0 && secs>HEARTBEAT_TIMEOUT ? " (STALE)" : "");
                }
            }
            pthread_mutex_unlock(&clients_lock);
        } else if (strncmp(line,"broadcast ",10)==0) {
            char *msg = line+10;
            char buf[MAXLINE];
            snprintf(buf, sizeof(buf), "BROADCAST|FromAdmin|Body:%s\n", msg);
            pthread_mutex_lock(&clients_lock);
            for (int i=0;i<MAX_CLIENTS;i++) {
                if (clients[i].udp_addr.sin_family == AF_INET) {
                    ssize_t s = sendto(udpfd, buf, strlen(buf), 0, (struct sockaddr*)&clients[i].udp_addr, sizeof(clients[i].udp_addr));
                    if (s<0) {
                        log_time_prefix(); printf("Failed to send UDP to %s: %s\n", clients[i].campus, strerror(errno));
                    } else {
                        log_time_prefix(); printf("Broadcasted to %s (%s:%d)\n", clients[i].campus, inet_ntoa(clients[i].udp_addr.sin_addr), ntohs(clients[i].udp_addr.sin_port));
                    }
                }
            }
            pthread_mutex_unlock(&clients_lock);
        } else if (strncmp(line,"sendcmd ",8)==0) {
            // send an ADMINCMD via TCP to a campus
            char *rest = line+8;
            char target[64] = {0};
            char payload[1024] = {0};
            if (sscanf(rest, "%63s %[^\n]", target, payload) >= 1) {
                int idx = find_client_index_by_campus(target);
                if (idx==-1 || clients[idx].tcp_fd==0) {
                    printf("Campus %s not connected via TCP.\n", target);
                } else {
                    char out[MAXLINE];
                    snprintf(out, sizeof(out), "ADMINCMD|Body:%s\n", payload);
                    if (send_all(clients[idx].tcp_fd, out, strlen(out)) < 0) {
                        printf("Failed to send ADMINCMD to %s. Removing.\n", target);
                        remove_client_index(idx);
                    } else {
                        printf("Sent ADMINCMD to %s\n", target);
                    }
                }
            } else {
                printf("Usage: sendcmd <Campus> <text>\n");
            }
        } else if (strncmp(line,"quit",4)==0) {
            log_time_prefix(); printf("Shutting down server by admin request.\n");
            exit(0);
        } else if (strlen(line)==0) {
            continue;
        } else {
            printf("Unknown command.\n");
        }
    }
}

int main() {
    memset(clients, 0, sizeof(clients));
    // TCP listen socket
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); exit(1); }
    int on=1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in servaddr;
    memset(&servaddr,0,sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(TCP_PORT);
    if (bind(listenfd, (struct sockaddr*)&servaddr, sizeof(servaddr))<0) { perror("bind"); exit(1); }
    if (listen(listenfd, 10) < 0) { perror("listen"); exit(1); }
    log_time_prefix(); printf("TCP server listening on port %d\n", TCP_PORT);

    // UDP socket
    int udpfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpfd < 0) { perror("udp socket"); exit(1); }
    struct sockaddr_in udpserv;
    memset(&udpserv,0,sizeof(udpserv));
    udpserv.sin_family = AF_INET;
    udpserv.sin_addr.s_addr = INADDR_ANY;
    udpserv.sin_port = htons(UDP_PORT);
    if (bind(udpfd, (struct sockaddr*)&udpserv, sizeof(udpserv)) < 0) { perror("udp bind"); exit(1); }
    log_time_prefix(); printf("UDP server listening on port %d\n", UDP_PORT);

    pthread_t t_accept, t_udp;
    pthread_create(&t_accept, NULL, tcp_accept_thread, &listenfd);
    pthread_detach(t_accept);
    pthread_create(&t_udp, NULL, udp_listener_thread, &udpfd);
    pthread_detach(t_udp);

    admin_console_loop(udpfd);

    close(listenfd);
    close(udpfd);
    return 0;
}
