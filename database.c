#include "database.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

sqlite3 *db;
pthread_mutex_t db_mutex = PTHREAD_MUTEX_INITIALIZER;

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
        return 0; // User already exists
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
}
