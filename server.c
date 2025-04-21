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
#include "client.h"
#include "database.h"
#include "room.h"

static int uid = 10;

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

        int receive = recv(cli->sockfd, buff_out, BUFFER_SZ, 0);
        if (receive > 0) {
            str_trim_lf(buff_out, strlen(buff_out));

            if (is_spamming(cli)) {
                char msg[64];
                snprintf(msg, sizeof(msg), "[Server] Spam detected! Blocked.\n");
                if (cli->sockfd > 0) {
                    send(cli->sockfd, msg, strlen(msg), 0);
                }
                continue;
            }

            if (strncmp(buff_out, "/create ", 8) == 0) {
                char room_name[32], users[BUFFER_SZ];
                sscanf(buff_out + 8, "%s %[^\n]", room_name, users);

                char sql_count[BUFFER_SZ];
                snprintf(sql_count, sizeof(sql_count), "SELECT COUNT(*) FROM rooms;");

                sqlite3_stmt *stmt_count = NULL;
                pthread_mutex_lock(&db_mutex);
                int rc = sqlite3_prepare_v2(db, sql_count, -1, &stmt_count, NULL);
                if (rc != SQLITE_OK || stmt_count == NULL) {
                    fprintf(stderr, "Failed to prepare count query: %s\n", sqlite3_errmsg(db));
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
                pthread_mutex_unlock(&db_mutex);

                if (room_count >= MAX_ROOMS) {
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
                pthread_mutex_lock(&db_mutex);
                rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                if (rc != SQLITE_OK || stmt == NULL) {
                    fprintf(stderr, "Failed to prepare room creation: %s\n", sqlite3_errmsg(db));
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
                pthread_mutex_unlock(&db_mutex);

                strcpy(cli->current_room, room_name);
                char msg[64];
                snprintf(msg, sizeof(msg), "[Server] Created room '%s'\n", room_name);
                if (cli->sockfd > 0) {
                    send(cli->sockfd, msg, strlen(msg), 0);
                }

                fprintf(stderr, "Sending history for room '%s' to user '%s' after creation\n", room_name, cli->name);
                send_history_to_client(cli->sockfd, room_name);

                snprintf(msg, sizeof(msg), "[Server] %s has created and joined room '%s'\n", cli->name, room_name);
                send_message_to_room(msg, room_name, cli->uid);
                continue;
            }

            if (strncmp(buff_out, "/join ", 6) == 0) {
                char room_name[32];
                sscanf(buff_out + 6, "%s", room_name);
                fprintf(stderr, "User '%s' requesting to join room '%s'\n", cli->name, room_name);

                pthread_mutex_lock(&db_mutex);
                char sql[BUFFER_SZ];
                snprintf(sql, sizeof(sql), "SELECT created_by FROM rooms WHERE name = ?;");
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
                sqlite3_finalize(stmt);

                if (!room_exists) {
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Room '%s' does not exist.\n", room_name);
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    continue;
                }

                snprintf(sql, sizeof(sql), "SELECT username FROM room_members WHERE room_name = ? AND username = ?;");
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
                    strcpy(cli->current_room, room_name);
                    pthread_mutex_unlock(&db_mutex);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "[Server] Joined room '%s'\n", room_name);
                    if (cli->sockfd > 0) {
                        send(cli->sockfd, msg, strlen(msg), 0);
                    }
                    snprintf(msg, sizeof(msg), "[Server] %s has joined room '%s'\n", cli->name, room_name);
                    send_message_to_room(msg, room_name, cli->uid);
                    fprintf(stderr, "Sending history for room '%s' to user '%s'\n", room_name, cli->name);
                    send_history_to_client(cli->sockfd, room_name);
                    continue;
                }

                snprintf(sql, sizeof(sql), "SELECT username FROM pending_requests WHERE room_name = ? AND username = ?;");
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
                sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
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
                snprintf(msg, sizeof(msg), "[Server] Join request sent for room '%s'.\n", room_name);
                if (cli->sockfd > 0) {
                    send(cli->sockfd, msg, strlen(msg), 0);
                }
                notify_room_members(room_name, cli->name);
                continue;
            }

            if (strncmp(buff_out, "/accept ", 8) == 0) {
                char room_name[32], username[32];
                sscanf(buff_out + 8, "%s %s", room_name, username);
            
                char sql[BUFFER_SZ];
                snprintf(sql, sizeof(sql), "SELECT username FROM room_members WHERE room_name = ? AND username = ?;");
                sqlite3_stmt *stmt = NULL;
                pthread_mutex_lock(&db_mutex);
                int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
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
            
                snprintf(sql, sizeof(sql), "SELECT username FROM pending_requests WHERE room_name = ? AND username = ?;");
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
                sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC);
                rc = sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            
                snprintf(sql, sizeof(sql), "DELETE FROM pending_requests WHERE room_name = ? AND username = ?;");
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
                snprintf(msg, sizeof(msg), "[Server] Accepted '%s' into room '%s'.\n", username, room_name);
                if (cli->sockfd > 0) {
                    send(cli->sockfd, msg, strlen(msg), 0);
                }
            
                pthread_mutex_lock(&clients_mutex);
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    if (clients[i] && strcmp(clients[i]->name, username) == 0) {
                        snprintf(msg, sizeof(msg), "[Server] Your join request for room '%s' has been accepted.\n", room_name);
                        if (clients[i]->sockfd > 0) {
                            send(clients[i]->sockfd, msg, strlen(msg), 0);
                        }
                        strcpy(clients[i]->current_room, room_name);
                        snprintf(msg, sizeof(msg), "[Server] %s has joined room '%s'\n", username, room_name);
                        send_message_to_room(msg, room_name, clients[i]->uid);
                        send_history_to_client(clients[i]->sockfd, room_name);
                        break;
                    }
                }
                pthread_mutex_unlock(&clients_mutex);
                continue;
            }

            if (strncmp(buff_out, "/reject ", 8) == 0) {
                char room_name[32], username[32];
                sscanf(buff_out + 8, "%s %s", room_name, username);

                char sql[BUFFER_SZ];
                snprintf(sql, sizeof(sql), "SELECT username FROM room_members WHERE room_name = ? AND username = ?;");
                sqlite3_stmt *stmt = NULL;
                pthread_mutex_lock(&db_mutex);
                int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
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

                snprintf(sql, sizeof(sql), "SELECT username FROM pending_requests WHERE room_name = ? AND username = ?;");
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

                snprintf(sql, sizeof(sql), "DELETE FROM pending_requests WHERE room_name = ? AND username = ?;");
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
                snprintf(msg, sizeof(msg), "[Server] Rejected '%s' from joining room '%s'.\n", username, room_name);
                if (cli->sockfd > 0) {
                    send(cli->sockfd, msg, strlen(msg), 0);
                }

                pthread_mutex_lock(&clients_mutex);
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    if (clients[i] && strcmp(clients[i]->name, username) == 0) {
                        snprintf(msg, sizeof(msg), "[Server] Your join request for room '%s' has been rejected.\n", room_name);
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
                char msg[64];
                snprintf(msg, sizeof(msg), "[Server] %s has left room '%s'\n", cli->name, cli->current_room);
                send_message_to_room(msg, cli->current_room, cli->uid);
                cli->current_room[0] = '\0';
                snprintf(msg, sizeof(msg), "[Server] Left room.\n");
                if (cli->sockfd > 0) {
                    send(cli->sockfd, msg, strlen(msg), 0);
                }

                fprintf(stderr, "Sending public room history to user '%s'\n", cli->name);
                send_history_to_client(cli->sockfd, "");
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

                snprintf(sql, sizeof(sql), "SELECT name, created_by FROM rooms;");
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
                    const char *creator = (const char *)sqlite3_column_text(stmt, 1);
                    char room_info[128];
                    int is_member = (strstr(member_rooms, name) != NULL);
                    int is_current = (strcmp(name, cli->current_room) == 0);
                    snprintf(room_info, sizeof(room_info), "  %s (Creator: %s, %s)\n", name, creator,
                             is_current ? "joined" : (is_member ? "member" : "not joined"));
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

            if (buff_out[0] != '/') {
                time_t now = time(NULL);
                struct tm *t = localtime(&now);
                char ts[64];
                strftime(ts, sizeof(ts), "[%Y-%m-%d %H:%M:%S]", t);

                char msg[BUFFER_SZ + 100];
                snprintf(msg, sizeof(msg), "%s %s: %s\n", ts, cli->name, buff_out);
                send_message_to_room(msg, cli->current_room, cli->uid);
                save_message_to_history(cli->current_room, cli->name, buff_out);
                printf("%s", msg);
            }
        } else if (receive == 0 || strcmp(buff_out, "exit") == 0) {
            if (cli->current_room[0] != '\0') {
                char msg[64];
                snprintf(msg, sizeof(msg), "[Server] %s has left room '%s'\n", cli->name, cli->current_room);
                send_message_to_room(msg, cli->current_room, cli->uid);
            }

            snprintf(buff_out, sizeof(buff_out), "[Server] %s has left\n", cli->name);
            printf("%s", buff_out);
            broadcast_status(buff_out);
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

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (!init_database()) {
        fprintf(stderr, "Failed to initialize database\n");
        return EXIT_FAILURE;
    }

    int port = atoi(argv[1]);
    int option = 1;
    int listenfd = 0, connfd = 0;
    struct sockaddr_in serv_addr, cli_addr;
    pthread_t tid;

    signal(SIGPIPE, SIG_IGN);
    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    if (bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Bind failed");
        sqlite3_close(db);
        return EXIT_FAILURE;
    }

    if (listen(listenfd, 10) < 0) {
        perror("Listen failed");
        sqlite3_close(db);
        return EXIT_FAILURE;
    }

    printf("=== CHATROOM SERVER STARTED ===\n");

    while (1) {
        socklen_t clilen = sizeof(cli_addr);
        connfd = accept(listenfd, (struct sockaddr*)&cli_addr, &clilen);

        if ((cli_count + 1) > MAX_CLIENTS) {
            printf("Max clients. Connection rejected.\n");
            close(connfd);
            continue;
        }

        client_t *cli = (client_t *)malloc(sizeof(client_t));
        cli->address = cli_addr;
        cli->sockfd = connfd;
        cli->uid = uid++;

        queue_add(cli);
        pthread_create(&tid, NULL, &handle_client, (void*)cli);
    }

    sqlite3_close(db);
    return EXIT_SUCCESS;
}
