#ifndef DATABASE_H
#define DATABASE_H

#include <sqlite3.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include "config.h"

extern sqlite3 *db;
extern pthread_mutex_t db_mutex;

int init_database();
int check_credentials(const char* username, const char* password);
int register_user(const char* username, const char* password);
void save_message_to_history(const char *room, const char *username, const char *msg);
void send_history_to_client(int sockfd, const char *room);

#endif
