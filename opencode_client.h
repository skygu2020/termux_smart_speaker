#ifndef OPENCODE_CLIENT_H
#define OPENCODE_CLIENT_H

/*
 * OpenCode HTTP API client for Termux Smart Speaker.
 * The reply carries the exact OpenCode session/message identity so every
 * TTS producer can use the same server-side idempotency key.
 */
typedef struct {
    char *text;
    char *session_id;
    char *message_id;
} OpenCodeReply;

int opencode_init(void);
int opencode_send_message(const char *message, OpenCodeReply *reply);
void opencode_reply_free(OpenCodeReply *reply);
void opencode_close(void);

#endif
