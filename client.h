#ifndef CLIENT_H
#define CLIENT_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include "config.h"

typedef struct {
    struct sockaddr_in address;
    int sockfd;
    int uid;
    char name[32];
    char current_room[32];
    time_t msg_times[SPAM_LIMIT];
    int msg_index;
    time_t block_until;
} client_t;

extern client_t *clients[MAX_CLIENTS];
extern pthread_mutex_t clients_mutex;
extern _Atomic unsigned int cli_count;
extern volatile sig_atomic_t flag; // Khai báo extern cho flag

void str_trim_lf(char* arr, int length);
void queue_add(client_t *cl);
void queue_remove(int uid);
int is_spamming(client_t *cli);
void str_overwrite_stdout(int is_blocked);
void catch_ctrl_c_and_exit(int sig);
void format_message(char *input, char *output, int max_len);

#endif
