#include "zeroclaw_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>
#include <cjson/cJSON.h>

// ============= 配置 =============
#define BUFFER_SIZE 65536
#define MAX_RESPONSE_SIZE 65536

// ============= 全局状态 =============
static int g_sock = -1;
static char g_host[256] = "127.0.0.1";
static int g_port = 42617;
static char g_token[512] = {0};
static char g_session_id[128] = "c-session-1";
static int g_timeout = 60;  // 60秒超时
static pthread_mutex_t g_send_mutex = PTHREAD_MUTEX_INITIALIZER;

// 同步响应
static char g_last_response[MAX_RESPONSE_SIZE] = {0};
static int g_response_ready = 0;
static pthread_mutex_t g_response_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_response_cond = PTHREAD_COND_INITIALIZER;

static int g_async_pending = 0;

// ============= WebSocket 辅助函数 =============
static void generate_websocket_key(char* key, size_t size) {
    const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 24; i++) {
        key[i] = chars[rand() % 64];
    }
    key[24] = '\0';
}

static int websocket_handshake(int sock, const char* host, int port, const char* token) {
    char key[32];
    char request[2048];
    char path[256];
    
    generate_websocket_key(key, sizeof(key)); 
    
    //snprintf(path, sizeof(path), "/ws/chat?session_id=%04x%04x-%04x-%04x-%04x-%04x%04x%04x",
    //    rand() & 0xffff, rand() & 0xffff,
    //    rand() & 0xffff,
    //    rand() & 0xffff,
    //    rand() & 0xffff,
    //    rand() & 0xffff, rand() & 0xffff, rand() & 0xffff);
    snprintf(path, sizeof(path), "/ws/chat?session_id=%s", g_session_id);
    
    int pos = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: zeroclaw.v1\r\n",
        path, host, port, key);
    
    if (token && strlen(token) > 0) {
        pos += snprintf(request + pos, sizeof(request) - pos, 
                        "Authorization: Bearer %s\r\n", token);
    }
    
    snprintf(request + pos, sizeof(request) - pos, "\r\n");
    
    printf("[ZWS] 发送握手请求\n");
    
    if (write(sock, request, strlen(request)) < 0) {
        perror("write");
        return -1;
    }
    
    char response[1024];
    int total = 0;
    while (total < sizeof(response) - 1) {
        int n = read(sock, response + total, 1);
        if (n <= 0) break;
        total++;
        if (total >= 4 && strstr(response, "\r\n\r\n")) break;
    }
    response[total] = '\0';
    
    if (strstr(response, "101 Switching Protocols") == NULL) {
        printf("[ZWS] ❌ 握手失败\n");
        return -1;
    }
    
    printf("[ZWS] ✅ 握手成功\n");
    return 0;
}

static int websocket_send_frame(int sock, const void* payload, size_t len, int opcode) {
    unsigned char header[14];
    size_t header_len = 2;
    
    header[0] = 0x80 | opcode;
    
    if (len < 126) {
        header[1] = 0x80 | len;
        header_len = 2;
    } else if (len < 65536) {
        header[1] = 0x80 | 126;
        header[2] = (len >> 8) & 0xFF;
        header[3] = len & 0xFF;
        header_len = 4;
    } else {
        return -1;
    }
    
    unsigned char mask[4];
    for (int i = 0; i < 4; i++) {
        mask[i] = rand() & 0xFF;
    }
    memcpy(header + header_len, mask, 4);
    header_len += 4;
    
    unsigned char* frame = (unsigned char*)malloc(header_len + len);
    if (!frame) return -1;
    
    memcpy(frame, header, header_len);
    memcpy(frame + header_len, payload, len);
    
    for (size_t i = 0; i < len; i++) {
        frame[header_len + i] ^= mask[i % 4];
    }
    
    int ret = write(sock, frame, header_len + len);
    free(frame);
    return ret;
}

static int websocket_send_text(int sock, const char* text) {
    return websocket_send_frame(sock, text, strlen(text), 0x01);
}

static int websocket_recv_frame(int sock, char* buffer, size_t buffer_size) {
    unsigned char header[2];
    if (read(sock, header, 2) != 2) return -1;
    
    int masked = (header[1] & 0x80);
    size_t payload_len = header[1] & 0x7F;
    
    if (payload_len == 126) {
        unsigned char len_bytes[2];
        if (read(sock, len_bytes, 2) != 2) return -1;
        payload_len = (len_bytes[0] << 8) | len_bytes[1];
    } else if (payload_len == 127) {
        return -1;
    }
    
    unsigned char mask[4];
    if (masked) {
        if (read(sock, mask, 4) != 4) return -1;
    }
    
    if (payload_len >= buffer_size) {
        payload_len = buffer_size - 1;
    }
    
    if (read(sock, buffer, payload_len) != (ssize_t)payload_len) return -1;
    buffer[payload_len] = '\0';
    
    if (masked) {
        for (size_t i = 0; i < payload_len; i++) {
            buffer[i] ^= mask[i % 4];
        }
    }
    
    return payload_len;
}

// ============= 提取最终响应 =============
static char* extract_full_response(const char* json_str) {
    cJSON* json = cJSON_Parse(json_str);
    if (!json) return NULL;
    
    char* content = NULL;
    cJSON* type = cJSON_GetObjectItem(json, "type");
    
    if (type && cJSON_IsString(type) && strcmp(type->valuestring, "done") == 0) {
        cJSON* full_response = cJSON_GetObjectItem(json, "full_response");
        if (full_response && cJSON_IsString(full_response)) {
            content = strdup(full_response->valuestring);
        }
    }
    
    cJSON_Delete(json);
    return content;
}

// ============= 接收线程 =============
static void* receive_thread(void* arg) {
    char buffer[MAX_RESPONSE_SIZE];
    
    printf("[ZWS] 接收线程启动\n");
    
    while (g_sock > 0) {
        int n = websocket_recv_frame(g_sock, buffer, sizeof(buffer));
        if (n <= 0) {
            printf("[ZWS] 🔌 连接关闭\n");
            break;
        }
        
        buffer[n] = '\0';
        
        // 只处理 done 消息
        char* content = extract_full_response(buffer);
        if (content) {
            //printf("[ZWS] 📥 最终响应: %s\n", content);
            
            pthread_mutex_lock(&g_response_mutex);
            strncpy(g_last_response, content, MAX_RESPONSE_SIZE - 1);
            g_response_ready = 1;
            if (g_async_pending) {
                pthread_cond_signal(&g_response_cond);
            }
            pthread_mutex_unlock(&g_response_mutex);
            free(content);
        }
    }
    
    printf("[ZWS] 接收线程退出\n");
    return NULL;
}

// ============= 公开 API =============
int zeroclaw_init(const char* host, int port, const char* token, const char* session_id) {
    srand(time(NULL));
    
    if (host) strncpy(g_host, host, sizeof(g_host) - 1);
    if (port > 0) g_port = port;
    if (token) strncpy(g_token, token, sizeof(g_token) - 1);
    if (session_id) strncpy(g_session_id, session_id, sizeof(g_session_id) - 1);
    
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) return -1;
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_port);
    addr.sin_addr.s_addr = inet_addr(g_host);
    
    printf("[ZWS] 连接 %s:%d ...\n", g_host, g_port);
    
    if (connect(g_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(g_sock);
        return -1;
    }
    
    if (websocket_handshake(g_sock, g_host, g_port, g_token) < 0) {
        close(g_sock);
        return -1;
    }
    
    pthread_t tid;
    pthread_create(&tid, NULL, receive_thread, NULL);
    pthread_detach(tid);
    
    printf("[ZWS] ✅ 已连接\n");
    return 0;
}

char* zeroclaw_send_message(const char* message) {
    if (!message || g_sock < 0) return NULL;
    
    pthread_mutex_lock(&g_send_mutex);
    
    //char msg[8192];
    //snprintf(msg, sizeof(msg), 
    //    "{\"type\":\"message\",\"content\":\"%s\",\"enable_think\":false,\"reasoning\":\"none\"}",
    //    message);
        
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "message");
    cJSON_AddStringToObject(root, "content", message);
    cJSON_AddFalseToObject(root, "enable_thinking");
    cJSON_AddStringToObject(root, "reasoning", "none");
    cJSON_AddStringToObject(root, "session_id", g_session_id);
    
    
    char* msg = cJSON_PrintUnformatted(root);
    
    pthread_mutex_lock(&g_response_mutex);
    g_response_ready = 0;
    g_last_response[0] = '\0';
    g_async_pending = 1;
    pthread_mutex_unlock(&g_response_mutex);
    
    if (websocket_send_text(g_sock, msg) < 0) {
        pthread_mutex_unlock(&g_send_mutex);
        return NULL;
    }
    
    printf("[ZWS] 📤 发送: %s\n", message);
    
    pthread_mutex_lock(&g_response_mutex);
    while (!g_response_ready && g_async_pending) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += g_timeout;
        pthread_cond_timedwait(&g_response_cond, &g_response_mutex, &ts);
    }
    
    char* response = g_response_ready ? strdup(g_last_response) : NULL;
    g_async_pending = 0;
    pthread_mutex_unlock(&g_response_mutex);
    
    pthread_mutex_unlock(&g_send_mutex);
    return response;
}

int zeroclaw_send_message_async(const char* message, ZeroClawCallback callback, void* userdata) {
    return -1;  // 暂不实现
}

void zeroclaw_close(void) {
    if (g_sock > 0) {
        close(g_sock);
        g_sock = -1;
    }
}

bool zeroclaw_is_connected(void) {
    return g_sock > 0;
}

void zeroclaw_set_timeout(int seconds) {
    if (seconds > 0) g_timeout = seconds;
}