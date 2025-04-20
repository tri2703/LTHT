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

#define LENGTH 2048

volatile sig_atomic_t flag = 0;
int sockfd = 0;
char name[32];
int is_blocked = 0;

void str_trim_lf(char* arr, int length) {
    for (int i = 0; i < length; i++) {
        if (arr[i] == '\n') {
            arr[i] = '\0';
            break;
        }
    }
}

void str_overwrite_stdout() {
    if (!is_blocked) {
        printf("> ");
        fflush(stdout);
    }
}

void catch_ctrl_c_and_exit(int sig) {
    flag = 1;
}

// Hàm xử lý định dạng tin nhắn và emoji
void format_message(char *input, char *output, int max_len) {
    char *pos = input;
    char *out = output;
    int out_len = 0;

    while (*pos && out_len < max_len - 10) {
        if (*pos == '*' && *(pos + 1) != '*' && *(pos + 1) != '\0') {
            // Bắt đầu in đậm
            if (strncmp(pos, "*text*", 6) != 0) {
                strcpy(out + out_len, "\033[1m");
                out_len += strlen("\033[1m");
                pos++;
                while (*pos != '*' && *pos && out_len < max_len - 10) {
                    out[out_len++] = *pos++;
                }
                if (*pos == '*') {
                    strcpy(out + out_len, "\033[0m");
                    out_len += strlen("\033[0m");
                    pos++;
                }
                continue;
            }
        } else if (*pos == '_' && *(pos + 1) != '_' && *(pos + 1) != '\0') {
            // Bắt đầu in nghiêng
            strcpy(out + out_len, "\033[3m");
            out_len += strlen("\033[3m");
            pos++;
            while (*pos != '_' && *pos && out_len < max_len - 10) {
                out[out_len++] = *pos++;
            }
            if (*pos == '_') {
                strcpy(out + out_len, "\033[0m");
                out_len += strlen("\033[0m");
                pos++;
            }
            continue;
        } else if (*pos == ':' && strncmp(pos, ":smile:", 7) == 0) {
            // Emoji smile
            strcpy(out + out_len, "😊");
            out_len += strlen("😊");
            pos += 7;
            continue;
        } else if (*pos == ':' && strncmp(pos, ":heart:", 7) == 0) {
            // Emoji heart
            strcpy(out + out_len, "❤️");
            out_len += strlen("❤️");
            pos += 7;
            continue;
        }
        out[out_len++] = *pos++;
    }
    out[out_len] = '\0';
}

void send_msg_handler() {
    char message[LENGTH] = {};
    char buffer[LENGTH + 32] = {};

    while (1) {
        if (is_blocked) {
            usleep(100000);
            continue;
        }

        str_overwrite_stdout();
        fgets(message, LENGTH, stdin);
        str_trim_lf(message, LENGTH);

        if (strlen(message) == 0) continue;

        // Handle exit
        if (strcmp(message, "exit") == 0) break;

        // Handle local commands
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

        // Send command or message to server
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

            // Log received message
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
                // Xử lý định dạng tin nhắn
                format_message(message, formatted_message, LENGTH + 100);
                printf("%s", formatted_message);
            }
            fflush(stdout); // Ensure message is displayed immediately
            str_overwrite_stdout();
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
    if (pthread_create(&send_msg_thread, NULL, (void *) send_msg_handler, NULL) != 0) {
        printf("ERROR: pthread\n");
        return EXIT_FAILURE;
    }

    pthread_t recv_msg_thread;
    if (pthread_create(&recv_msg_thread, NULL, (void *) recv_msg_handler, NULL) != 0) {
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
