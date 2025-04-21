#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "client.h"

int sockfd = 0;
char name[32];
int is_blocked = 0;

void send_msg_handler() {
    char message[LENGTH] = {};
    char buffer[LENGTH + 32] = {};

    while (1) {
        if (is_blocked) {
            usleep(100000);
            continue;
        }

        str_overwrite_stdout(is_blocked);
        fgets(message, LENGTH, stdin);
        str_trim_lf(message, LENGTH);

        if (strlen(message) == 0) continue;

        if (strcmp(message, "exit") == 0) break;

        if (strcmp(message, "/help") == 0) {
            printf("Available commands:\n");
            printf("  /create <room> <user1> <user2> ... : Create private room\n");
            printf("  /join <room>                     : Send request to join a private room\n");
            printf("  /accept <room> <user>            : Accept a user's join request (admin only)\n");
            printf("  /reject <room> <user>            : Reject a user's join request (admin only)\n");
            printf("  /leave                           : Leave current room\n");
            printf("  /rooms                           : List available rooms\n");
            printf("  /online                          : List online users\n");
            printf("  exit                             : Quit chat\n");
            printf("Formatting:\n");
            printf("  *text*                           : Bold text\n");
            printf("  _text_                           : Italic text\n");
            printf("  :smile:                          : Smile emoji 😊\n");
            printf("  :heart:                          : Heart emoji ❤️\n");
            continue;
        }

        snprintf(buffer, sizeof(buffer), "%s", message);
        send(sockfd, buffer, strlen(buffer), 0);

        bzero(message, LENGTH);
        bzero(buffer, LENGTH + 32);
    }

    catch_ctrl_c_and_exit(2);
}

void recv_msg_handler() {
    char message[LENGTH] = {};

    char formatted_message[LENGTH + 100] = {};
    FILE *log_file = fopen("client_log.txt", "a");
    if (!log_file) {
        perror("Failed to open client_log.txt");
    }

    while (1) {
        int receive = recv(sockfd, message, LENGTH - 1, 0);
        if (receive > 0) {
            message[receive] = '\0';

            if (log_file) {
                fprintf(log_file, "Received: %s", message);
                fflush(log_file);
            }

            if (strstr(message, "Spam detected")) {
                is_blocked = 1;
                printf("\033[1;31m%s\033[0m", message);
            } else if (strstr(message, "no longer blocked")) {
                is_blocked = 0;
                printf("\033[1;32m%s\033[0m", message);
            } else {
                format_message(message, formatted_message, LENGTH + 100);
                printf("%s", formatted_message);
            }
            fflush(stdout);
            str_overwrite_stdout(is_blocked);
        } else if (receive == 0) {
            break;
        }
        memset(message, 0, sizeof(message));
        memset(formatted_message, 0, sizeof(formatted_message));
    }

    if (log_file) fclose(log_file);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("Usage: %s <server_ip> <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *ip = argv[1];
    int port = atoi(argv[2]);

    signal(SIGINT, catch_ctrl_c_and_exit);

    char action[10];
    int choice = 0;

    while (choice != 1 && choice != 2) {
        printf("Select an action:\n");
        printf("  1. Login\n");
        printf("  2. Register\n");
        printf("Your choice: ");
        fgets(action, sizeof(action), stdin);
        choice = atoi(action);
        if (choice != 1 && choice != 2) {
            printf("Invalid option. Please enter 1 or 2.\n");
        }
    }

    strcpy(action, (choice == 1) ? "login" : "register");

    printf("Username: ");
    fgets(name, 32, stdin);
    str_trim_lf(name, 32);

    char password[32];
    printf("Password: ");
    fgets(password, 32, stdin);
    str_trim_lf(password, 32);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(ip);
    server_addr.sin_port = htons(port);

    int err = connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (err == -1) {
        perror("ERROR: connect");
        return EXIT_FAILURE;
    }

    char auth_message[100];
    snprintf(auth_message, sizeof(auth_message), "%s|%s|%s", action, name, password);
    send(sockfd, auth_message, strlen(auth_message), 0);

    char auth_response[LENGTH];
    int bytes = recv(sockfd, auth_response, sizeof(auth_response) - 1, 0);
    if (bytes <= 0) {
        printf("Disconnected or failed to receive response.\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    auth_response[bytes] = '\0';

    char *newline = strchr(auth_response, '\n');
    if (newline) {
        *newline = '\0';
        printf("%s\n", auth_response);
        printf("%s\n", newline + 1);
    } else {
        printf("%s\n", auth_response);
    }

    if (strcmp(auth_response, "OK") != 0) {
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("=== WELCOME TO THE CHATROOM ===\n");
    printf("Type /help to see available commands.\n");

    pthread_t send_msg_thread;
    if (pthread_create(&send_msg_thread, NULL, (void *)send_msg_handler, NULL) != 0) {
        printf("ERROR: pthread\n");
        return EXIT_FAILURE;
    }

    pthread_t recv_msg_thread;
    if (pthread_create(&recv_msg_thread, NULL, (void *)recv_msg_handler, NULL) != 0) {
        printf("ERROR: pthread\n");
        return EXIT_FAILURE;
    }

    while (1) {
        if (flag) {
            printf("\nBye\n");
            break;
        }
    }

    close(sockfd);
    return EXIT_SUCCESS;
}
