// Run: ./client <CampusName> <Password> <ServerIP>
// Example: ./client Lahore NU-LHR-123 127.0.0.1

#define _POSIX_C_SOURCE 200809L
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

#define TCP_PORT 9000
#define UDP_PORT 9001
#define MAXLINE 2048
#define HEARTBEAT_INTERVAL 10 // seconds

char campus_name[64];
char campus_pass[128];
char server_ip[64];

int tcp_fd = -1;
int udp_fd = -1;
struct sockaddr_in server_udp_addr;

pthread_mutex_t recv_lock = PTHREAD_MUTEX_INITIALIZER;

void trim_newline(char *s) {
    char *p = strchr(s,'\r');
    if (p) *p = 0;
    p = strchr(s,'\n');
    if (p) *p = 0;
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

void *tcp_recv_thread(void *arg) {
    (void)arg;
    char buf[MAXLINE];
    ssize_t n;
    char inbuf[MAXLINE];
    size_t inpos = 0;
    while (1) {
        n = recv(tcp_fd, buf, sizeof(buf)-1, 0);
        if (n <= 0) {
            printf("\nDisconnected from server (TCP). Exiting.\n");
            exit(1);
        }
        for (ssize_t i=0;i<n;i++) {
            inbuf[inpos++] = buf[i];
            if (inpos >= sizeof(inbuf)-1) inpos = sizeof(inbuf)-2;
            if (buf[i] == '\n') {
                inbuf[inpos]=0;
                trim_newline(inbuf);
                // handle lines
                if (strncmp(inbuf,"FROM|",5)==0) {
                    char fromCampus[64]="", fromDept[64]="", body[MAXLINE]="";
                    char *p = strdup(inbuf+5);
                    char *tok = strtok(p,"|");
                    while (tok) {
                        if (strncmp(tok,"FromCampus:",11)==0) strncpy(fromCampus, tok+11, sizeof(fromCampus)-1);
                        if (strncmp(tok,"FromDept:",9)==0) strncpy(fromDept, tok+9, sizeof(fromDept)-1);
                        if (strncmp(tok,"Body:",5)==0) strncpy(body, tok+5, sizeof(body)-1);
                        tok = strtok(NULL,"|");
                    }
                    free(p);
                    printf("\n--- MESSAGE from %s (%s) ---\n%s\n-----------------------------\n> ", fromCampus, fromDept, body);
                    fflush(stdout);
                } else if (strncmp(inbuf,"ADMINCMD|",9)==0) {
                    char body[MAXLINE]="";
                    char *p = strdup(inbuf+9);
                    char *tok = strtok(p,"|");
                    while (tok) {
                        if (strncmp(tok,"Body:",5)==0) strncpy(body, tok+5, sizeof(body)-1);
                        tok = strtok(NULL,"|");
                    }
                    free(p);
                    printf("\n--- ADMINCMD ---\n%s\n----------------\n> ", body);
                    fflush(stdout);
                } else {
                    printf("\n[SERVER] %s\n> ", inbuf);
                    fflush(stdout);
                }
                inpos = 0;
            }
        }
    }
    return NULL;
}

void *udp_recv_thread(void *arg) {
    (void)arg;
    char buf[MAXLINE];
    struct sockaddr_in src;
    socklen_t len = sizeof(src);
    while (1) {
        ssize_t n = recvfrom(udp_fd, buf, sizeof(buf)-1, 0, (struct sockaddr*)&src, &len);
        if (n<0) { perror("recvfrom"); continue; }
        buf[n]=0;
        // expect BROADCAST|FromAdmin|Body:...
        if (strncmp(buf,"BROADCAST|",10)==0) {
            char body[MAXLINE]="";
            char *p = strdup(buf+10);
            char *tok = strtok(p,"|");
            while (tok) {
                if (strncmp(tok,"Body:",5)==0) strncpy(body, tok+5, sizeof(body)-1);
                tok = strtok(NULL,"|");
            }
            free(p);
            printf("\n--- BROADCAST ---\n%s\n-----------------\n> ", body);
            fflush(stdout);
        } else {
            printf("\n[UDP] %s\n> ", buf);
            fflush(stdout);
        }
    }
    return NULL;
}

void *heartbeat_thread(void *arg) {
    int local_udp_port = *((int*)arg);
    free(arg);
    while (1) {
        char hb[MAXLINE];
        snprintf(hb, sizeof(hb), "HEARTBEAT|Campus:%s|UDPPort:%d\n", campus_name, local_udp_port);
        ssize_t s = sendto(udp_fd, hb, strlen(hb), 0, (struct sockaddr*)&server_udp_addr, sizeof(server_udp_addr));
        if (s<0) {
            perror("sendto heartbeat");
        } else {
            //printf("[heartbeat sent]\n");
        }
        sleep(HEARTBEAT_INTERVAL);
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <CampusName> <Password> <ServerIP>\nExample: %s Lahore NU-LHR-123 127.0.0.1\n", argv[0], argv[0]);
        exit(1);
    }
    strncpy(campus_name, argv[1], sizeof(campus_name)-1);
    strncpy(campus_pass, argv[2], sizeof(campus_pass)-1);
    strncpy(server_ip, argv[3], sizeof(server_ip)-1);

    // create TCP socket and connect
    tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd < 0) { perror("socket"); exit(1); }
    struct sockaddr_in serv;
    memset(&serv,0,sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_port = htons(TCP_PORT);
    if (inet_pton(AF_INET, server_ip, &serv.sin_addr) <= 0) { perror("inet_pton"); exit(1); }
    if (connect(tcp_fd, (struct sockaddr*)&serv, sizeof(serv)) < 0) { perror("connect"); exit(1); }
    printf("Connected to server %s:%d via TCP\n", server_ip, TCP_PORT);

    // perform AUTH
    char auth[MAXLINE];
    snprintf(auth, sizeof(auth), "AUTH|Campus:%s|Pass:%s\n", campus_name, campus_pass);
    if (send_all(tcp_fd, auth, strlen(auth)) < 0) { perror("send auth"); exit(1); }
    // wait for response
    char resp[256];
    ssize_t r = recv(tcp_fd, resp, sizeof(resp)-1, 0);
    if (r <= 0) { perror("recv auth"); exit(1); }
    resp[r]=0;
    trim_newline(resp);
    if (strcmp(resp,"AUTH_OK")!=0) {
        fprintf(stderr, "Authentication failed: %s\n", resp);
        close(tcp_fd);
        exit(1);
    }
    printf("Authenticated successfully as %s\n", campus_name);

    // setup UDP socket; bind to any available port so we can receive broadcasts
    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) { perror("udp socket"); exit(1); }
    struct sockaddr_in local_udp;
    memset(&local_udp,0,sizeof(local_udp));
    local_udp.sin_family = AF_INET;
    local_udp.sin_addr.s_addr = INADDR_ANY;
    local_udp.sin_port = 0; // let OS choose
    if (bind(udp_fd, (struct sockaddr*)&local_udp, sizeof(local_udp)) < 0) { perror("bind udp"); exit(1); }
    struct sockaddr_in actual;
    socklen_t alen = sizeof(actual);
    getsockname(udp_fd, (struct sockaddr*)&actual, &alen);
    int local_port = ntohs(actual.sin_port);
    printf("UDP socket bound to port %d (listening for broadcasts)\n", local_port);

    memset(&server_udp_addr,0,sizeof(server_udp_addr));
    server_udp_addr.sin_family = AF_INET;
    server_udp_addr.sin_port = htons(UDP_PORT);
    if (inet_pton(AF_INET, server_ip, &server_udp_addr.sin_addr) <= 0) { perror("inet_pton server udp"); exit(1); }

    // start threads: TCP recv, UDP recv, heartbeat
    pthread_t t_tcp, t_udp, t_hb;
    pthread_create(&t_tcp, NULL, tcp_recv_thread, NULL);
    pthread_create(&t_udp, NULL, udp_recv_thread, NULL);
    int *pport = malloc(sizeof(int));
    *pport = local_port;
    pthread_create(&t_hb, NULL, heartbeat_thread, pport);

    pthread_detach(t_tcp);
    pthread_detach(t_udp);
    pthread_detach(t_hb);

    // console loop
    char line[2048];
    while (1) {
        printf("\n--- %s Console ---\n", campus_name);
        printf("1) send <TargetCampus> <TargetDept> <FromDept> <Message>\n");
        printf("2) quit\n> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        trim_newline(line);
        if (strncmp(line,"quit",4)==0 || strcmp(line,"2")==0) {
            printf("Exiting.\n");
            close(tcp_fd);
            close(udp_fd);
            exit(0);
        } else if (strncmp(line,"send ",5)==0 || line[0]=='1') {
            char tmp[2048];
            if (line[0]=='1') {
                // prompt
                char target[64], todept[64], fromdept[64], body[1024];
                printf("TargetCampus: "); fflush(stdout); if (!fgets(tmp,sizeof(tmp),stdin)) break; trim_newline(tmp); strncpy(target,tmp,sizeof(target)-1);
                printf("TargetDept: "); fflush(stdout); if (!fgets(tmp,sizeof(tmp),stdin)) break; trim_newline(tmp); strncpy(todept,tmp,sizeof(todept)-1);
                printf("FromDept: "); fflush(stdout); if (!fgets(tmp,sizeof(tmp),stdin)) break; trim_newline(tmp); strncpy(fromdept,tmp,sizeof(fromdept)-1);
                printf("Message: "); fflush(stdout); if (!fgets(tmp,sizeof(tmp),stdin)) break; trim_newline(tmp); strncpy(body,tmp,sizeof(body)-1);
                snprintf(tmp, sizeof(tmp), "MSG|ToCampus:%s|ToDept:%s|FromDept:%s|Body:%s\n", target, todept, fromdept, body);
                if (send_all(tcp_fd, tmp, strlen(tmp)) < 0) { perror("send msg"); }
            } else {
                // parse inline: send <TargetCampus> <TargetDept> <FromDept> <Message>
                char target[64], todept[64], fromdept[64], body[1024];
                // find first 3 tokens then the rest is message
                char *p = line + 5;
                while (*p == ' ') p++;
                if (sscanf(p, "%63s %63s %63s %[^\n]", target, todept, fromdept, body) >= 4) {
                    snprintf(tmp, sizeof(tmp), "MSG|ToCampus:%s|ToDept:%s|FromDept:%s|Body:%s\n", target, todept, fromdept, body);
                    if (send_all(tcp_fd, tmp, strlen(tmp)) < 0) { perror("send msg"); }
                } else {
                    printf("Usage: send <TargetCampus> <TargetDept> <FromDept> <Message>\n");
                }
            }
        } else {
            printf("Unknown command.\n");
        }
    }

    return 0;
}
