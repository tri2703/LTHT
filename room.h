#ifndef ROOM_H
#define ROOM_H

#include "client.h"
#include "config.h"

void send_message_to_room(const char *s, const char *room, int exclude_uid);
void send_online_users(int sockfd);
void broadcast_status(char *msg);
void notify_room_members(const char *room_name, const char *username);

#endif
