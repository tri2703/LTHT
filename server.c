#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <signal.h>

#define MAX_CLIENTS 100
#define BUFFER_SZ 2048

static _Atomic unsigned int cli_count = 0;
static int uid = 10;

typedef struct {
    struct sockaddr_in address;
    int sockfd;
    int uid;
    char name[32];
} client_t;

client_t *clients[MAX_CLIENTS];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

void str_trim_lf(char* arr, int length) {
    for (int i = 0; i < length; i++) {
        if (arr[i] == '\n') {
            arr[i] = '\0';
            break;
        }
    }
}

int check_credentials(const char* username, const char* password) {
    FILE *fp = fopen("accounts.txt", "r");
    if (!fp) return 0;
    char u[32], p[32];
    while (fscanf(fp, "%s %s", u, p) != EOF) {
        if (strcmp(u, username) == 0 && strcmp(p, password) == 0) {
            fclose(fp);
            return 1;
        }
    }
    fclose(fp);
    return 0;
}

int register_user(const char* username, const char* password) {
    FILE *fp_check = fopen("accounts.txt", "r");
    if (fp_check) {
        char u[32], p[32];
        while (fscanf(fp_check, "%s %s", u, p) != EOF) {
            if (strcmp(u, username) == 0) {
                fclose(fp_check);
                return 0;
            }
        }
        fclose(fp_check);
    }

    FILE *fp = fopen("accounts.txt", "a");
    if (!fp) return 0;
    fprintf(fp, "%s %s\n", username, password);
    fclose(fp);
    return 1;
}

void queue_add(client_t *cl){
    pthread_mutex_lock(&clients_mutex);
    for(int i=0; i < MAX_CLIENTS; ++i){
        if(!clients[i]){
            clients[i] = cl;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void queue_remove(int uid){
    pthread_mutex_lock(&clients_mutex);
    for(int i=0; i < MAX_CLIENTS; ++i){
        if(clients[i]){
            if(clients[i]->uid == uid){
                clients[i] = NULL;
                break;
            }
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void send_message(char *s, int uid){
    pthread_mutex_lock(&clients_mutex);
    for(int i=0; i<MAX_CLIENTS; ++i){
        if(clients[i]){
            if(clients[i]->uid != uid){
                if(write(clients[i]->sockfd, s, strlen(s)) < 0){
                    perror("ERROR: write to descriptor failed");
                    break;
                }
            }
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void *handle_client(void *arg){
    char buff_out[BUFFER_SZ];
    int leave_flag = 0;

    cli_count++;
    client_t *cli = (client_t *)arg;

    char buffer[100];
    if (recv(cli->sockfd, buffer, sizeof(buffer), 0) <= 0) {
        leave_flag = 1;
    } else {
        // Parse: action|username|password
        char *action = strtok(buffer, "|");
        char *username = strtok(NULL, "|");
        char *password = strtok(NULL, "|");

        if (!action || !username || !password) {
            send(cli->sockfd, "Invalid format", 14, 0);
            leave_flag = 1;
        } else if (strcmp(action, "login") == 0) {
            if (check_credentials(username, password)) {
                strcpy(cli->name, username);
                send(cli->sockfd, "OK", 2, 0);
                sprintf(buff_out, "%s has joined\n", cli->name);
                printf("%s", buff_out);
                send_message(buff_out, cli->uid);
            } else {
                send(cli->sockfd, "Login failed", 13, 0);
                leave_flag = 1;
            }
        } else if (strcmp(action, "register") == 0) {
            if (register_user(username, password)) {
                strcpy(cli->name, username);
                send(cli->sockfd, "OK", 2, 0);
                sprintf(buff_out, "%s has registered and joined\n", cli->name);
                printf("%s", buff_out);
                send_message(buff_out, cli->uid);
            } else {
                send(cli->sockfd, "User already exists", 20, 0);
                leave_flag = 1;
            }
        } else {
            send(cli->sockfd, "Unknown action", 14, 0);
            leave_flag = 1;
        }
    }

    bzero(buff_out, BUFFER_SZ);

    while(1){
        if (leave_flag) break;

        int receive = recv(cli->sockfd, buff_out, BUFFER_SZ, 0);
        if (receive > 0){
            if(strlen(buff_out) > 0){
                send_message(buff_out, cli->uid);
                str_trim_lf(buff_out, strlen(buff_out));
                printf("%s -> %s\n", buff_out, cli->name);
            }
        } else {
            sprintf(buff_out, "%s has left\n", cli->name);
            printf("%s", buff_out);
            send_message(buff_out, cli->uid);
            leave_flag = 1;
        }
        bzero(buff_out, BUFFER_SZ);
    }

    close(cli->sockfd);
    queue_remove(cli->uid);
    free(cli);
    cli_count--;
    pthread_detach(pthread_self());

    return NULL;
}

int main(int argc, char **argv){
    if(argc != 2){
        printf("Usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *ip = "0.0.0.0"; // cho phép mọi client kết nối
    int port = atoi(argv[1]);
    int option = 1;
    int listenfd = 0, connfd = 0;
    struct sockaddr_in serv_addr, cli_addr;
    pthread_t tid;

    signal(SIGPIPE, SIG_IGN);
    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(ip);
    serv_addr.sin_port = htons(port);

    if(setsockopt(listenfd, SOL_SOCKET,(SO_REUSEPORT | SO_REUSEADDR), (char*)&option, sizeof(option)) < 0){
        perror("ERROR: setsockopt failed");
        return EXIT_FAILURE;
    }

    if(bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("ERROR: Socket binding failed");
        return EXIT_FAILURE;
    }

    if (listen(listenfd, 10) < 0) {
        perror("ERROR: Socket listening failed");
        return EXIT_FAILURE;
    }

    printf("=== CHATROOM SERVER STARTED ===\n");

    while(1){
        socklen_t clilen = sizeof(cli_addr);
        connfd = accept(listenfd, (struct sockaddr*)&cli_addr, &clilen);

        if((cli_count + 1) == MAX_CLIENTS){
            printf("Max clients reached. Rejecting...\n");
            close(connfd);
            continue;
        }

        client_t *cli = (client_t *)malloc(sizeof(client_t));
        cli->address = cli_addr;
        cli->sockfd = connfd;
        cli->uid = uid++;

        queue_add(cli);
        pthread_create(&tid, NULL, &handle_client, (void*)cli);
        sleep(1);
    }

    return EXIT_SUCCESS;
}
