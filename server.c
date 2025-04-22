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
#include <time.h>
#include <ctype.h>
#include <sqlite3.h>

#define MAX_CLIENTS 100
#define BUFFER_SZ 2048
#define SPAM_LIMIT 10
#define SPAM_INTERVAL 8
#define BLOCK_DURATION 15
#define MAX_ROOMS 20

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

static _Atomic unsigned int cli_count = 0;
static int uid = 10;
client_t *clients[MAX_CLIENTS];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t db_mutex = PTHREAD_MUTEX_INITIALIZER;
sqlite3 *db;

void str_trim_lf(char* arr, int length) {
    for (int i = 0; i < length; i++) {
        if (arr[i] == '\n') {
            arr[i] = '\0';
            break;
        }
    }
}

int check_credentials(const char* username, const char* password) {
    char sql[BUFFER_SZ];
    snprintf(sql, sizeof(sql), "SELECT password FROM users WHERE username = ?;");

    sqlite3_stmt *stmt = NULL;
    pthread_mutex_lock(&db_mutex);
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK || stmt == NULL) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&db_mutex);
        return 0;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    int found = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *stored_password = (const char *)sqlite3_column_text(stmt, 0);
        if (strcmp(stored_password, password) == 0) {
            found = 1;
        }
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_mutex);
    return found;
}

int register_user(const char* username, const char* password) {
    char sql[BUFFER_SZ];
    snprintf(sql, sizeof(sql), "SELECT username FROM users WHERE username = ?;");

    sqlite3_stmt *stmt = NULL;
    pthread_mutex_lock(&db_mutex);
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK || stmt == NULL) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&db_mutex);
        return 0;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&db_mutex);
        return 0;
    }
    sqlite3_finalize(stmt);

    snprintf(sql, sizeof(sql), "INSERT INTO users (username, password) VALUES (?, ?);");
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK || stmt == NULL) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&db_mutex);
        return 0;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, password, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Failed to execute statement: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&db_mutex);
        return 0;
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_mutex);
    return 1;
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

void send_message(char *s, int uid) {
    client_t *sender = NULL;

    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] && clients[i]->uid == uid) {
            sender = clients[i];
            break;
        }
    }

    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] && clients[i]->uid != uid) {
            if (strcmp(clients[i]->current_room, sender->current_room) == 0) {
                if (clients[i]->sockfd > 0) {
                    if (write(clients[i]->sockfd, s, strlen(s)) < 0) {
                        perror("ERROR: write failed");
                        break;
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

int init_database() {
    int rc = sqlite3_open("chat_history.db", &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    const char *sql = "CREATE TABLE IF NOT EXISTS users ("
                      "username TEXT PRIMARY KEY,"
                      "password TEXT NOT NULL);"
                      "CREATE TABLE IF NOT EXISTS rooms ("
                      "name TEXT PRIMARY KEY,"
                      "created_by TEXT NOT NULL);"
                      "CREATE TABLE IF NOT EXISTS room_members ("
                      "room_name TEXT,"
                      "username TEXT,"
                      "PRIMARY KEY (room_name, username),"
                      "FOREIGN KEY (room_name) REFERENCES rooms(name),"
                      "FOREIGN KEY (username) REFERENCES users(username));"
                      "CREATE TABLE IF NOT EXISTS messages ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                      "room TEXT NOT NULL,"
                      "timestamp TEXT NOT NULL,"
                      "username TEXT NOT NULL,"
                      "message TEXT NOT NULL);"
                      "CREATE TABLE IF NOT EXISTS pending_requests ("
                      "room_name TEXT,"
                      "username TEXT,"
                      "request_time TEXT NOT NULL,"
                      "PRIMARY KEY (room_name, username),"
                      "FOREIGN KEY (room_name) REFERENCES rooms(name),"
                      "FOREIGN KEY (username) REFERENCES users(username));";
    char *err_msg = 0;

    pthread_mutex_lock(&db_mutex);
    rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        pthread_mutex_unlock(&db_mutex);
        sqlite3_close(db);
        return 0;
    }
    pthread_mutex_unlock(&db_mutex);

    FILE *fp = fopen("accounts.txt", "r");
    if (fp) {
        char username[32], password[32];
        while (fscanf(fp, "%s %s", username, password) != EOF) {
            char sql_insert[BUFFER_SZ];
            snprintf(sql_insert, sizeof(sql_insert), "INSERT OR IGNORE INTO users (username, password) VALUES (?, ?);");

            sqlite3_stmt *stmt = NULL;
            pthread_mutex_lock(&db_mutex);
            if (sqlite3_prepare_v2(db, sql_insert, -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, password, -1, SQLITE_STATIC);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
            pthread_mutex_unlock(&db_mutex);
        }
        fclose(fp);
    }

    return 1;
}

void save_message_to_history(const char *room, const char *username, const char *msg) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[20];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);

    char sql[BUFFER_SZ];
    snprintf(sql, sizeof(sql), "INSERT INTO messages (room, timestamp, username, message) VALUES (?, ?, ?, ?);");

    sqlite3_stmt *stmt = NULL;
    pthread_mutex_lock(&db_mutex);
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK || stmt == NULL) {
        fprintf(stderr, "Failed to prepare statement for saving message: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&db_mutex);
        return;
    }

    sqlite3_bind_text(stmt, 1, room, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, ts, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, msg, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Failed to save message to room '%s' by '%s': %s\n", room, username, sqlite3_errmsg(db));
    } else {
        fprintf(stderr, "Saved message to room '%s' by '%s': %s\n", room, username, msg);
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_mutex);
}

void send_history_to_client(int sockfd, const char *room) {
    fprintf(stderr, "Querying history for room '%s'\n", room[0] ? room : "public");

    char sql[BUFFER_SZ];
    snprintf(sql, sizeof(sql), "SELECT timestamp, username, message FROM messages WHERE room = ? ORDER BY id;");

    sqlite3_stmt *stmt = NULL;
    pthread_mutex_lock(&db_mutex);
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK || stmt == NULL) {
        fprintf(stderr, "Failed to prepare history query for room '%s': %s\n", room, sqlite3_errmsg(db));
        pthread_mutex_unlock(&db_mutex);
        if (sockfd > 0) {
            send(sockfd, "[Server] Error retrieving history.\n", 35, 0);
        }
        return;
    }

    sqlite3_bind_text(stmt, 1, room, -1, SQLITE_STATIC);

    int msg_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *ts = (const char *)sqlite3_column_text(stmt, 0);
        const char *username = (const char *)sqlite3_column_text(stmt, 1);
        const char *message = (const char *)sqlite3_column_text(stmt, 2);

        char line[BUFFER_SZ];
        snprintf(line, sizeof(line), "[%s] %s: %s\n", ts, username, message);
        if (sockfd > 0) {
            if (send(sockfd, line, strlen(line), 0) <= 0) {
                fprintf(stderr, "Failed to send history message to client for room '%s': %s\n", room, line);
                break;
            }
        }
        msg_count++;
        fprintf(stderr, "Sent message %d: %s", msg_count, line);
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_mutex);
    fprintf(stderr, "Sent %d history messages for room '%s' to client\n", msg_count, room[0] ? room : "public");

    usleep(200000);
}

void *handle_client(void *arg) {
    char buff_out[BUFFER_SZ];
    int leave_flag = 0;
    client_t *cli = (client_t *)arg;
    cli_count++;
    memset(cli->msg_times, 0, sizeof(cli->msg_times));
    cli->msg_index = 0;
    cli->block_until = 0;
    cli->current_room[0] = '\0';

    char buffer[100];
    if (recv(cli->sockfd, buffer, sizeof(buffer), 0) <= 0) {
        leave_flag = 1;
    } else {
        char *action = strtok(buffer, "|");
        char *username = strtok(NULL, "|");
        char *password = strtok(NULL, "|");

        if (!action || !username || !password) {
            if (cli->sockfd > 0) {
                send(cli->sockfd, "Invalid format", 14, 0);
            }
            leave_flag = 1;
        } else if (strcmp(action, "login") == 0) {
            if (check_credentials(username, password)) {
                strcpy(cli->name, username);
                if (cli->sockfd > 0) {
                    send(cli->sockfd, "OK\n", 3, 0);
                }
                usleep(100000);
                fprintf(stderr, "Sending public room history to user '%s'\n", cli->name);
                send_history_to_client(cli->sockfd, "");

                sprintf(buff_out, "[Server] %s has joined\n", cli->name);
                printf("%s", buff_out);
                broadcast_status(buff_out);
                send_online_users(cli->sockfd);
            } else {
                if (cli->sockfd > 0) {
                    send(cli->sockfd, "Login failed", 13, 0);
                }
                leave_flag = 1;
            }
        } else if (strcmp(action, "register") == 0) {
            if (register_user(username, password)) {
                strcpy(cli->name, username);
                if (cli->sockfd > 0) {
                    send(cli->sockfd, "OK\n", 3, 0);
                }
                sprintf(buff_out, "[Server] %s has registered and joined\n", cli->name);
                printf("%s", buff_out);
                broadcast_status(buff_out);
                send_online_users(cli->sockfd);
            } else {
                if (cli->sockfd > 0) {
                    send(cli->sockfd, "User already exists", 20, 0);
                }
                leave_flag = 1;
            }
        } else {
            if (cli->sockfd > 0) {
                send(cli->sockfd, "Unknown action", 14, 0);
            }
            leave_flag = 1;
        }
    }

    bzero(buff_out, BUFFER_SZ);

    while (!leave_flag) {
        time_t now = time(NULL);
        if (cli->block_until > now) {
            usleep(100000);
            continue;
        }

        if (cli->block_until != 0 && cli->block_until <= now) {
            char msg[64];
            snprintf(msg, sizeof(msg), "[Server] You are no longer blocked.\n");
            if (cli->sockfd > 0) {
                send(cli->sockfd, msg, strlen(msg), 0);
            }
            cli->block_until = 0;
            memset(cli->msg_times, 0, sizeof(cli->msg_times));
            cli->msg_index = 0;
        }

        bzero(buff_out, BUFFER_SZ);
        int receive = recv(cli->sockfd, buff_out, BUFFER_SZ - 1, 0);
        if (receive > 0) {
            buff_out[receive] = '\0';
            str_trim_lf(buff_out, receive);
            fprintf(stderr, "Received from user '%s': %s\n", cli->name, buff_out);

            if (is_spamming(cli)) {
                char msg[64];
                snprintf(msg, sizeof(msg), "[Server] Spam detected! Blocked.\n");
                if (cli->sockfd > 0) {
                    send(cli->sockfd, msg, strlen(msg), 0);
                }
                continue;
            }

            if (strncmp(buff_out, "/create ", 8) == 0) {
                char room_name[32] = {0}, users[BUFFER_SZ];
                sscanf(buff_out + 8, "%s %[^\n]", room_name, users);
                fprintf(stderr, "User '%s' creating room '%s'\n", cli->name, room_name);

                char sql_count[BUFFER_SZ];
                snprintf(sql_count, sizeof(sql_count), "SELECT COUNT(*) FROM rooms;");

                sqlite3_stmt *stmt_count = NULL;
                pthread_mutex_lock(&db_mutex);

                int rc = sqlite3_exec(db, "BEGIN TRANSACTION;", 0, 0, 0);
                if (rc != SQLITE_OK) {
                    fprintf(stderr, "Failed to begin transaction: %s\n", sqlite3_errmsg(db));
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Database error.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }

                rc = sqlite3_prepare_v2(db, sql_count, -1, &stmt_count, NULL);
                if (rc != SQLITE_OK || stmt_count == NULL) {
                    fprintf(stderr, "Failed to prepare count query: %s\n", sqlite3_errmsg(db));
                    sqlite3_exec(db, "ROLLBACK;", 0, 0, 0);
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Database error.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }
                sqlite3_step(stmt_count);
                int room_count = sqlite3_column_int(stmt_count, 0);
                sqlite3_finalize(stmt_count);

                if (room_count >= MAX_ROOMS) {
                    sqlite3_exec(db, "ROLLBACK;", 0, 0, 0);
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Max room limit.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }

                char sql[BUFFER_SZ];
                snprintf(sql, sizeof(sql), "INSERT INTO rooms (name, created_by) VALUES (?, ?);");
                sqlite3_stmt *stmt = NULL;
                rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                if (rc != SQLITE_OK || stmt == NULL) {
                    fprintf(stderr, "Failed to prepare room creation: %s\n", sqlite3_errmsg(db));
                    sqlite3_exec(db, "ROLLBACK;", 0, 0, 0);
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Database error.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }
                sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, cli->name, -1, SQLITE_STATIC);
                rc = sqlite3_step(stmt);
                sqlite3_finalize(stmt);
                if (rc != SQLITE_DONE) {
                    sqlite3_exec(db, "ROLLBACK;", 0, 0, 0);
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Room already exists.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }

                snprintf(sql, sizeof(sql), "INSERT INTO room_members (room_name, username) VALUES (?, ?);");
                rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                if (rc == SQLITE_OK && stmt != NULL) {
                    sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
                    sqlite3_bind_text(stmt, 2, cli->name, -1, SQLITE_STATIC);
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }

                char *token = strtok(users, " ");
                while (token) {
                    snprintf(sql, sizeof(sql), "INSERT OR IGNORE INTO room_members (room_name, username) VALUES (?, ?);");
                    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                    if (rc == SQLITE_OK && stmt != NULL) {
                        sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
                        sqlite3_bind_text(stmt, 2, token, -1, SQLITE_STATIC);
                        sqlite3_step(stmt);
                        sqlite3_finalize(stmt);
                    }
                    token = strtok(NULL, " ");
                }

                rc = sqlite3_exec(db, "COMMIT;", 0, 0, 0);
                if (rc != SQLITE_OK) {
                    fprintf(stderr, "Failed to commit transaction: %s\n", sqlite3_errmsg(db));
                    sqlite3_exec(db, "ROLLBACK;", 0, 0, 0);
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Database error.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }

                pthread_mutex_unlock(&db_mutex);

                strcpy(cli->current_room, room_name);
                char msg[64];
                snprintf(msg, sizeof(msg), "[Server] Created room '%s'\n", room_name);
                if (cli->sockfd > 0) {
                    send(cli->sockfd, msg, strlen(msg), 0);
                }

                fprintf(stderr, "Sending history for room '%s' to user '%s' after creation\n", room_name, cli->name);
                send_history_to_client(cli->sockfd, room_name);
                continue;
            }

            if (strncmp(buff_out, "/join ", 6) == 0) {
                char room_name[32] = {0};
                sscanf(buff_out + 6, "%s", room_name);
                fprintf(stderr, "User '%s' requesting to join room '%s'\n", cli->name, room_name);

                pthread_mutex_lock(&db_mutex);
                char sql[BUFFER_SZ];
                snprintf(sql, sizeof(sql), "SELECT created_by FROM rooms WHERE LOWER(name) = LOWER(?);");
                sqlite3_stmt *stmt = NULL;
                int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                if (rc != SQLITE_OK || stmt == NULL) {
                    fprintf(stderr, "Failed to prepare room existence check: %s\n", sqlite3_errmsg(db));
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Database error.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }
                sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
                int room_exists = (sqlite3_step(stmt) == SQLITE_ROW);
                char original_room_name[32] = {0};
                if (room_exists) {
                    snprintf(sql, sizeof(sql), "SELECT name FROM rooms WHERE LOWER(name) = LOWER(?);");
                    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                    if (rc == SQLITE_OK && stmt != NULL) {
                        sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
                        if (sqlite3_step(stmt) == SQLITE_ROW) {
                            const char *name = (const char *)sqlite3_column_text(stmt, 0);
                            strncpy(original_room_name, name, sizeof(original_room_name) - 1);
                        }
                    }
                    sqlite3_finalize(stmt);
                } else {
                    sqlite3_finalize(stmt);
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Room '%s' does not exist.\n", room_name);
                    fprintf(stderr, "Room check failed for '%s'\n", room_name);
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }

                snprintf(sql, sizeof(sql), "SELECT username FROM room_members WHERE LOWER(room_name) = LOWER(?) AND username = ?;");
                rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                if (rc != SQLITE_OK || stmt == NULL) {
                    fprintf(stderr, "Failed to prepare membership check: %s\n", sqlite3_errmsg(db));
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Database error.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }
                sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, cli->name, -1, SQLITE_STATIC);
                int is_member = (sqlite3_step(stmt) == SQLITE_ROW);
                sqlite3_finalize(stmt);

                if (is_member) {
                    strcpy(cli->current_room, original_room_name);
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Joined room '%s'\n", original_room_name);
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    fprintf(stderr, "Sending history for room '%s' to user '%s'\n", original_room_name, cli->name);
                    send_history_to_client(cli->sockfd, original_room_name);
                    usleep(500000);
                    continue;
                }

                snprintf(sql, sizeof(sql), "SELECT username FROM pending_requests WHERE LOWER(room_name) = LOWER(?) AND username = ?;");
                rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                if (rc != SQLITE_OK || stmt == NULL) {
                    fprintf(stderr, "Failed to prepare pending request check: %s\n", sqlite3_errmsg(db));
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Database error.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }
                sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, cli->name, -1, SQLITE_STATIC);
                int request_exists = (sqlite3_step(stmt) == SQLITE_ROW);
                sqlite3_finalize(stmt);

                if (request_exists) {
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Join request already pending for room '%s'.\n", room_name);
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }

                snprintf(sql, sizeof(sql), "SELECT username FROM users WHERE username = ?;");
                rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                if (rc != SQLITE_OK || stmt == NULL) {
                    fprintf(stderr, "Failed to prepare user check: %s\n", sqlite3_errmsg(db));
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Database error.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }
                sqlite3_bind_text(stmt, 1, cli->name, -1, SQLITE_STATIC);
                int user_exists = (sqlite3_step(stmt) == SQLITE_ROW);
                sqlite3_finalize(stmt);

                if (!user_exists) {
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] User '%s' not found.\n", cli->name);
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }

                time_t now = time(NULL);
                struct tm *t = localtime(&now);
                char ts[20];
                strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);
                snprintf(sql, sizeof(sql), "INSERT INTO pending_requests (room_name, username, request_time) VALUES (?, ?, ?);");
                rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                if (rc != SQLITE_OK || stmt == NULL) {
                    fprintf(stderr, "Failed to prepare join request insertion: %s\n", sqlite3_errmsg(db));
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Database error.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }
                sqlite3_bind_text(stmt, 1, original_room_name, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, cli->name, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 3, ts, -1, SQLITE_STATIC);
                rc = sqlite3_step(stmt);
                if (rc != SQLITE_DONE) {
                    fprintf(stderr, "Failed to insert join request: %s\n", sqlite3_errmsg(db));
                    sqlite3_finalize(stmt);
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Failed to send join request.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }
                sqlite3_finalize(stmt);
                pthread_mutex_unlock(&db_mutex);

                char msg[64];
                snprintf(msg, sizeof(msg), "[Server] Join request sent for room '%s'.\n", original_room_name);
                if (cli->sockfd > 0) {
                    send(cli->sockfd, msg, strlen(msg), 0);
                }
                notify_room_members(original_room_name, cli->name);
                continue;
            }

            if (strncmp(buff_out, "/accept ", 8) == 0) {
                char room_name[32] = {0}, username[32];
                sscanf(buff_out + 8, "%s %s", room_name, username);
                fprintf(stderr, "User '%s' accepting '%s' into room '%s'\n", cli->name, username, room_name);

                char original_room_name[32] = {0};
                pthread_mutex_lock(&db_mutex);
                char sql[BUFFER_SZ];
                snprintf(sql, sizeof(sql), "SELECT name FROM rooms WHERE LOWER(name) = LOWER(?);");
                sqlite3_stmt *stmt = NULL;
                int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                if (rc == SQLITE_OK && stmt != NULL) {
                    sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
                    if (sqlite3_step(stmt) == SQLITE_ROW) {
                        const char *name = (const char *)sqlite3_column_text(stmt, 0);
                        strncpy(original_room_name, name, sizeof(original_room_name) - 1);
                    }
                }
                sqlite3_finalize(stmt);

                snprintf(sql, sizeof(sql), "SELECT username FROM room_members WHERE LOWER(room_name) = LOWER(?) AND username = ?;");
                rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                if (rc != SQLITE_OK || stmt == NULL) {
                    fprintf(stderr, "Failed to check room membership: %s\n", sqlite3_errmsg(db));
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Database error.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }
                sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, cli->name, -1, SQLITE_STATIC);
                int is_member = (sqlite3_step(stmt) == SQLITE_ROW);
                sqlite3_finalize(stmt);

                if (!is_member) {
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] You are not a member of room '%s'.\n", room_name);
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }

                snprintf(sql, sizeof(sql), "SELECT username FROM pending_requests WHERE LOWER(room_name) = LOWER(?) AND username = ?;");
                rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                if (rc != SQLITE_OK || stmt == NULL) {
                    fprintf(stderr, "Failed to check pending request: %s\n", sqlite3_errmsg(db));
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Database error.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }
                sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC);
                int request_exists = (sqlite3_step(stmt) == SQLITE_ROW);
                sqlite3_finalize(stmt);

                if (!request_exists) {
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] No pending request from '%s' for room '%s'.\n", username, room_name);
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }

                snprintf(sql, sizeof(sql), "INSERT INTO room_members (room_name, username) VALUES (?, ?);");
                rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                if (rc != SQLITE_OK || stmt == NULL) {
                    fprintf(stderr, "Failed to add user to room: %s\n", sqlite3_errmsg(db));
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Database error.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }
                sqlite3_bind_text(stmt, 1, original_room_name, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC);
                rc = sqlite3_step(stmt);
                sqlite3_finalize(stmt);

                snprintf(sql, sizeof(sql), "DELETE FROM pending_requests WHERE LOWER(room_name) = LOWER(?) AND username = ?;");
                rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                if (rc != SQLITE_OK || stmt == NULL) {
                    fprintf(stderr, "Failed to remove pending request: %s\n", sqlite3_errmsg(db));
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Database error.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }
                sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC);
                rc = sqlite3_step(stmt);
                sqlite3_finalize(stmt);
                pthread_mutex_unlock(&db_mutex);

                if (rc != SQLITE_DONE) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Failed to process accept request.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }

                char msg[64];
                snprintf(msg, sizeof(msg), "[Server] Accepted '%s' into room '%s'.\n", username, original_room_name);
                if (cli->sockfd > 0) {
                    send(cli->sockfd, msg, strlen(msg), 0);
                }

                pthread_mutex_lock(&clients_mutex);
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    if (clients[i] && strcmp(clients[i]->name, username) == 0) {
                        snprintf(msg, sizeof(msg), "[Server] Your join request for room '%s' has been accepted.\n", original_room_name);
                        if (clients[i]->sockfd > 0) {
                            send(clients[i]->sockfd, msg, strlen(msg), 0);
                        }
                        strcpy(clients[i]->current_room, original_room_name);
                        send_history_to_client(clients[i]->sockfd, original_room_name);
                        usleep(500000);
                        break;
                    }
                }
                pthread_mutex_unlock(&clients_mutex);
                continue;
            }

            if (strncmp(buff_out, "/reject ", 8) == 0) {
                char room_name[32] = {0}, username[32];
                sscanf(buff_out + 8, "%s %s", room_name, username);
                fprintf(stderr, "User '%s' rejecting '%s' from room '%s'\n", cli->name, username, room_name);

                char original_room_name[32] = {0};
                pthread_mutex_lock(&db_mutex);
                char sql[BUFFER_SZ];
                snprintf(sql, sizeof(sql), "SELECT name FROM rooms WHERE LOWER(name) = LOWER(?);");
                sqlite3_stmt *stmt = NULL;
                int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                if (rc == SQLITE_OK && stmt != NULL) {
                    sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
                    if (sqlite3_step(stmt) == SQLITE_ROW) {
                        const char *name = (const char *)sqlite3_column_text(stmt, 0);
                        strncpy(original_room_name, name, sizeof(original_room_name) - 1);
                    }
                }
                sqlite3_finalize(stmt);

                snprintf(sql, sizeof(sql), "SELECT username FROM room_members WHERE LOWER(room_name) = LOWER(?) AND username = ?;");
                rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                if (rc != SQLITE_OK || stmt == NULL) {
                    fprintf(stderr, "Failed to check room membership: %s\n", sqlite3_errmsg(db));
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Database error.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }
                sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, cli->name, -1, SQLITE_STATIC);
                int is_member = (sqlite3_step(stmt) == SQLITE_ROW);
                sqlite3_finalize(stmt);

                if (!is_member) {
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] You are not a member of room '%s'.\n", room_name);
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }

                snprintf(sql, sizeof(sql), "SELECT username FROM pending_requests WHERE LOWER(room_name) = LOWER(?) AND username = ?;");
                rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                if (rc != SQLITE_OK || stmt == NULL) {
                    fprintf(stderr, "Failed to check pending request: %s\n", sqlite3_errmsg(db));
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Database error.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }
                sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC);
                int request_exists = (sqlite3_step(stmt) == SQLITE_ROW);
                sqlite3_finalize(stmt);

                if (!request_exists) {
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] No pending request from '%s' for room '%s'.\n", username, room_name);
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }

                snprintf(sql, sizeof(sql), "DELETE FROM pending_requests WHERE LOWER(room_name) = LOWER(?) AND username = ?;");
                rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                if (rc != SQLITE_OK || stmt == NULL) {
                    fprintf(stderr, "Failed to remove pending request: %s\n", sqlite3_errmsg(db));
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Database error.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }
                sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC);
                rc = sqlite3_step(stmt);
                sqlite3_finalize(stmt);
                pthread_mutex_unlock(&db_mutex);

                if (rc != SQLITE_DONE) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Failed to process reject request.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }

                char msg[64];
                snprintf(msg, sizeof(msg), "[Server] Rejected '%s' from joining room '%s'.\n", username, original_room_name);
                if (cli->sockfd > 0) {
                    send(cli->sockfd, msg, strlen(msg), 0);
                }

                pthread_mutex_lock(&clients_mutex);
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    if (clients[i] && strcmp(clients[i]->name, username) == 0) {
                        snprintf(msg, sizeof(msg), "[Server] Your join request for room '%s' has been rejected.\n", original_room_name);
                        if (clients[i]->sockfd > 0) {
                            send(clients[i]->sockfd, msg, strlen(msg), 0);
                        }
                        break;
                    }
                }
                pthread_mutex_unlock(&clients_mutex);
                continue;
            }

            if (strcmp(buff_out, "/leave") == 0) {
                char leave_msg[BUFFER_SZ];
                snprintf(leave_msg, sizeof(leave_msg), "[Server] %s has left room '%s'.\n", cli->name, cli->current_room);
                send_message(leave_msg, cli->uid);

                cli->current_room[0] = '\0';
                char msg[64];
                snprintf(msg, sizeof(msg), "[Server] Left room.\n");
                if (cli->sockfd > 0) {
                    send(cli->sockfd, msg, strlen(msg), 0);
                }

                fprintf(stderr, "Sending public room history to user '%s'\n", cli->name);
                send_history_to_client(cli->sockfd, "");
                usleep(500000);
                continue;
            }

            if (strcmp(buff_out, "/rooms") == 0) {
                char room_list[BUFFER_SZ] = "[Server] Room Information:\n";
                int has_rooms = 0;

                char current_room_info[64];
                snprintf(current_room_info, sizeof(current_room_info), "You are in: %s\n",
                         cli->current_room[0] == '\0' ? "public" : cli->current_room);
                strcat(room_list, current_room_info);

                strcat(room_list, "All rooms:\n");

                snprintf(current_room_info, sizeof(current_room_info), "  public (%s)\n",
                         cli->current_room[0] == '\0' ? "joined" : "not joined");
                strcat(room_list, current_room_info);
                has_rooms = 1;

                char member_rooms[BUFFER_SZ] = {0};
                char sql[BUFFER_SZ];
                snprintf(sql, sizeof(sql), "SELECT room_name FROM room_members WHERE username = ?;");
                sqlite3_stmt *stmt = NULL;
                pthread_mutex_lock(&db_mutex);
                int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                if (rc != SQLITE_OK || stmt == NULL) {
                    fprintf(stderr, "Failed to prepare member rooms query: %s\n", sqlite3_errmsg(db));
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Database error.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }
                sqlite3_bind_text(stmt, 1, cli->name, -1, SQLITE_STATIC);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char *name = (const char *)sqlite3_column_text(stmt, 0);
                    char room_info[64];
                    snprintf(room_info, sizeof(room_info), "%s\n", name);
                    strcat(member_rooms, room_info);
                    has_rooms = 1;
                }
                sqlite3_finalize(stmt);

                snprintf(sql, sizeof(sql), "SELECT name FROM rooms;");
                rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                if (rc != SQLITE_OK || stmt == NULL) {
                    fprintf(stderr, "Failed to prepare all rooms query: %s\n", sqlite3_errmsg(db));
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Database error.\n");
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char *name = (const char *)sqlite3_column_text(stmt, 0);
                    char room_info[64];
                    int is_member = (strstr(member_rooms, name) != NULL);
                    int is_current = (strcmp(name, cli->current_room) == 0);
                    snprintf(room_info, sizeof(room_info), "  %s (%s)\n", name,
                             is_current ? "joined" : (is_member ? "joined" : "not joined"));
                    strcat(room_list, room_info);
                    has_rooms = 1;
                }
                sqlite3_finalize(stmt);
                pthread_mutex_unlock(&db_mutex);

                if (!has_rooms) {
                    strcat(room_list, "  (No rooms available)\n");
                }
                if (cli->sockfd > 0) {
                    send(cli->sockfd, room_list, strlen(room_list), 0);
                }
                continue;
            }

            if (strcmp(buff_out, "/online") == 0) {
                send_online_users(cli->sockfd);
                continue;
            }

            time_t now = time(NULL);
            struct tm *t = localtime(&now);
            char ts[64];
            strftime(ts, sizeof(ts), "[%Y-%m-%d %H:%M:%S]", t);

            char msg[BUFFER_SZ + 100] = {0};
            snprintf(msg, sizeof(msg), "%s %s: %s\n", ts, cli->name, buff_out);
            fprintf(stderr, "Sending message: %s", msg);
            send_message(msg, cli->uid);
            save_message_to_history(cli->current_room, cli->name, buff_out);
            printf("%s", msg);
        } else if (receive == 0 || strcmp(buff_out, "exit") == 0) {
            sprintf(buff_out, "[Server] %s has left\n", cli->name);
            printf("%s", buff_out);
            broadcast_status(buff_out);
            leave_flag = 1;
        } else {
            printf("ERROR: -1\n");
            leave_flag = 1;
        }
    }

    close(cli->sockfd);
    queue_remove(cli->uid);
    free(cli);
    cli_count--;
    pthread_detach(pthread_self());

    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *port = argv[1];
    struct sockaddr_in server_addr;
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        printf("ERROR: socket creation failed\n");
        return EXIT_FAILURE;
    }

    int optval = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        printf("ERROR: setsockopt failed\n");
        return EXIT_FAILURE;
    }

    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(atoi(port));

    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
        printf("ERROR: bind failed\n");
        return EXIT_FAILURE;
    }

    if (listen(sockfd, 10) != 0) {
        printf("ERROR: listen failed\n");
        return EXIT_FAILURE;
    }

    printf("=== WELCOME TO THE CHATROOM ===\n");

    if (!init_database()) {
        printf("ERROR: Database initialization failed\n");
        return EXIT_FAILURE;
    }

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int connfd = accept(sockfd, (struct sockaddr*)&client_addr, &client_len);
        if (connfd < 0) {
            perror("ERROR: accept failed");
            continue;
        }

        if (cli_count >= MAX_CLIENTS) {
            printf("Max clients reached. Connection rejected: %d\n", connfd);
            close(connfd);
            continue;
        }

        client_t *cli = (client_t *)malloc(sizeof(client_t));
        cli->address = client_addr;
        cli->sockfd = connfd;
        cli->uid = uid++;

        queue_add(cli);
        pthread_t tid;
        pthread_create(&tid, NULL, &handle_client, (void*)cli);
    }

    sqlite3_close(db);
    return EXIT_SUCCESS;
}