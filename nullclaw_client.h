#ifndef NULLCLAW_CLIENT_H
#define NULLCLAW_CLIENT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 NullClaw 客户端
int nullclaw_init(const char* host, int port, const char* token, const char* session_id);

// 发送消息到 NullClaw（阻塞，等待响应）
char* nullclaw_send_message(const char* message);

// 发送消息到 NullClaw（异步）
typedef void (*NullClawCallback)(const char* response, void* userdata);
int nullclaw_send_message_async(const char* message, NullClawCallback callback, void* userdata);

// 关闭连接
void nullclaw_close(void);

// 检查连接状态
bool nullclaw_is_connected(void);

// 设置超时时间（秒）
void nullclaw_set_timeout(int seconds);

#ifdef __cplusplus
}
#endif

#endif // NULLCLAW_CLIENT_H