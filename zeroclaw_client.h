#ifndef ZEROCLAW_CLIENT_H
#define ZEROCLAW_CLIENT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 ZeroClaw 客户端
int zeroclaw_init(const char* host, int port, const char* token, const char* session_id);

// 发送消息到 ZeroClaw（阻塞，等待响应）
char* zeroclaw_send_message(const char* message);

// 发送消息到 ZeroClaw（异步，通过回调返回）
typedef void (*ZeroClawCallback)(const char* response, void* userdata);
int zeroclaw_send_message_async(const char* message, ZeroClawCallback callback, void* userdata);

// 关闭连接
void zeroclaw_close(void);

// 检查连接状态
bool zeroclaw_is_connected(void);

// 设置超时时间（秒）
void zeroclaw_set_timeout(int seconds);

#ifdef __cplusplus
}
#endif

#endif // ZEROCLAW_CLIENT_H