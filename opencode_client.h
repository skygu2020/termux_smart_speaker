#ifndef OPENCODE_CLIENT_H
#define OPENCODE_CLIENT_H

/*
 * OpenCode HTTP API client for Termux Smart Speaker.
 *
 * The client connects only to the local opencode2 service. It never starts
 * another OpenCode process and never reuses unrelated project sessions.
 */
int opencode_init(void);
char *opencode_send_message(const char *message);
void opencode_close(void);

#endif
