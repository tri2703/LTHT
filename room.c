#include "room.h"
#include <stdio.h>
#include <string.h>
#include <sqlite3.h>

extern sqlite3 *db;
extern pthread_mutex_t db_mutex;

void send_message_to_room(const char *s, const char *room, int exclude_uid) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] && clients[i]->uid != exclude_uid) {
            if (strcmp(clients[i]->current_room, room) == 0) {
                if (clients[i]->sockfd > 0) {
                    if (write(clients[i]->sockfd, s, strlen(s)) < 0) {
                        perror("ERROR: write failed");
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void send_online_users(int sockfd) {
    char user_list[BUFFER_SZ] = "[Server] Online users:\n";
    int has_users = 0;

    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i]) {
            char user_info[64];
            snprintf(user_info, sizeof(user_info), "  %s\n", clients[i]->name);
            strcat(user_list, user_info);
            has_users = 1;
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    if (!has_users) {
        strcat(user_list, "  (No users online)\n");
    }
    if (sockfd > 0) {
        send(sockfd, user_list, strlen(user_list), 0);
    }
}

void broadcast_status(char *msg) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] && clients[i]->current_room[0] == '\0') {
            if (clients[i]->sockfd > 0) {
                if (write(clients[i]->sockfd, msg, strlen(msg)) < 0) {
                    perror("ERROR: write failed");
                }
            }
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void notify_room_members(const char *room_name, const char *username) {
    char sql[BUFFER_SZ];
    snprintf(sql, sizeof(sql), "SELECT username FROM room_members WHERE room_name = ?;");

    sqlite3_stmt *stmt = NULL;
    pthread_mutex_lock(&db_mutex);
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK || stmt == NULL) {
        fprintf(stderr, "Failed to prepare statement for room members lookup: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&db_mutex);
        return;
    }

    sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
    int members_found = 0;
    pthread_mutex_lock(&clients_mutex);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *temp_member = (const char *)sqlite3_column_text(stmt, 0);
        if (!temp_member) continue;
        char member[32] = {0};
        strncpy(member, temp_member, sizeof(member) - 1);

        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (clients[i] && strcmp(clients[i]->name, member) == 0 && clients[i]->sockfd > 0) {
                char msg[BUFFER_SZ];
                snprintf(msg, sizeof(msg), "[Server] %s has requested to join room '%s'. Use /accept %s %s or /reject %s %s\n",
                         username, room_name, room_name, username, room_name, username);
                if (write(clients[i]->sockfd, msg, strlen(msg)) < 0) {
                    perror("ERROR: Failed to notify room member");
                } else {
                    members_found++;
                }
                break;
            }
        }
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_mutex);
    pthread_mutex_unlock(&clients_mutex);

    if (!members_found) {
        fprintf(stderr, "No online members found for room '%s'\n", room_name);
    } else {
        fprintf(stderr, "Notified %d members for room '%s'\n", members_found, room_name);
    }
}
