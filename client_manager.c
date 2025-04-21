#include "client.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

client_t *clients[MAX_CLIENTS];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
_Atomic unsigned int cli_count = 0;
volatile sig_atomic_t flag = 0; // Định nghĩa flag ở đây

void str_trim_lf(char* arr, int length) {
    for (int i = 0; i < length; i++) {
        if (arr[i] == '\n') {
            arr[i] = '\0';
            break;
        }
    }
}

void queue_add(client_t *cl) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i]) {
            clients[i] = cl;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void queue_remove(int uid) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] && clients[i]->uid == uid) {
            clients[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

int is_spamming(client_t *cli) {
    time_t now = time(NULL);
    cli->msg_times[cli->msg_index] = now;
    cli->msg_index = (cli->msg_index + 1) % SPAM_LIMIT;

    int valid_msgs = 0;
    for (int i = 0; i < SPAM_LIMIT; i++) {
        if (cli->msg_times[i] != 0) valid_msgs++;
    }
    if (valid_msgs < SPAM_LIMIT) return 0;

    time_t oldest = now;
    for (int i = 0; i < SPAM_LIMIT; i++) {
        if (cli->msg_times[i] < oldest) oldest = cli->msg_times[i];
    }

    if (difftime(now, oldest) <= SPAM_INTERVAL) {
        cli->block_until = now + BLOCK_DURATION;
        return 1;
    }
    return 0;
}

void str_overwrite_stdout(int is_blocked) {
    if (!is_blocked) {
        printf("> ");
        fflush(stdout);
    }
}

void catch_ctrl_c_and_exit(int sig) {
    flag = 1;
}

void format_message(char *input, char *output, int max_len) {
    char *pos = input;
    char *out = output;
    int out_len = 0;

    while (*pos && out_len < max_len - 10) {
        if (*pos == '*' && *(pos + 1) != '*' && *(pos + 1) != '\0') {
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
            strcpy(out + out_len, "😊");
            out_len += strlen("😊");
            pos += 7;
            continue;
        } else if (*pos == ':' && strncmp(pos, ":heart:", 7) == 0) {
            strcpy(out + out_len, "❤️");
            out_len += strlen("❤️");
            pos += 7;
            continue;
        }
        out[out_len++] = *pos++;
    }
    out[out_len] = '\0';
}
