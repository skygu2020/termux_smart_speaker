#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/rand.h>

#include <cjson/cJSON.h>

#define WS_OPCODE_TEXT   0x1
#define WS_OPCODE_CLOSE  0x8
#define WS_OPCODE_PING   0x9
#define WS_OPCODE_PONG   0xA
#define WS_MASK_BIT      0x80

typedef struct {
    int sockfd;
    char session_id[128];
    char agent_id[128];
    char auth_token[512];
    int is_connected;
} ws_client_t;

// Base64 encoding (standard, not URL-safe for WebSocket)
char* base64_encode(const unsigned char* input, int len) {
    BIO *bio, *b64;
    BUF_MEM *buffer_ptr;
    
    b64 = BIO_new(BIO_f_base64());
    bio = BIO_new(BIO_s_mem());
    bio = BIO_push(b64, bio);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, input, len);
    BIO_flush(bio);
    BIO_get_mem_ptr(bio, &buffer_ptr);
    
    char* encoded = malloc(buffer_ptr->length + 1);
    memcpy(encoded, buffer_ptr->data, buffer_ptr->length);
    encoded[buffer_ptr->length] = '\0';
    
    BIO_free_all(bio);
    return encoded;
}

// Generate WebSocket key (base64, not URL-safe)
void generate_websocket_key(char* key, int key_len) {
    unsigned char random[16];
    RAND_bytes(random, sizeof(random));
    char* encoded = base64_encode(random, 16);
    strncpy(key, encoded, key_len - 1);
    key[key_len - 1] = '\0';
    free(encoded);
}

// Compute WebSocket accept key
void compute_accept_key(const char* client_key, char* accept_key, int accept_len) {
    char concat[256];
    snprintf(concat, sizeof(concat), "%s%s", client_key, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1((unsigned char*)concat, strlen(concat), hash);
    
    char* encoded = base64_encode(hash, SHA_DIGEST_LENGTH);
    strncpy(accept_key, encoded, accept_len - 1);
    accept_key[accept_len - 1] = '\0';
    free(encoded);
}

// Create WebSocket frame
int create_ws_frame(char* buffer, int opcode, const char* payload, int payload_len, int is_masked) {
    int header_len = 2;
    buffer[0] = 0x80 | (opcode & 0x0F);
    
    if (payload_len < 126) {
        buffer[1] = (is_masked ? WS_MASK_BIT : 0) | payload_len;
        header_len = 2;
    } else if (payload_len < 65536) {
        buffer[1] = (is_masked ? WS_MASK_BIT : 0) | 126;
        buffer[2] = (payload_len >> 8) & 0xFF;
        buffer[3] = payload_len & 0xFF;
        header_len = 4;
    } else {
        buffer[1] = (is_masked ? WS_MASK_BIT : 0) | 127;
        for (int i = 0; i < 8; i++) {
            buffer[2 + i] = (payload_len >> (56 - 8 * i)) & 0xFF;
        }
        header_len = 10;
    }
    
    int offset = header_len;
    unsigned char mask[4] = {0};
    
    if (is_masked) {
        RAND_bytes(mask, 4);
        memcpy(buffer + offset, mask, 4);
        offset += 4;
        for (int i = 0; i < payload_len; i++) {
            buffer[offset + i] = payload[i] ^ mask[i % 4];
        }
    } else {
        memcpy(buffer + offset, payload, payload_len);
    }
    
    return offset + payload_len;
}

// Parse WebSocket frame
int parse_ws_frame(const char* buffer, int len, char* payload, int* payload_len, int* opcode) {
    if (len < 2) return -1;
    
    *opcode = buffer[0] & 0x0F;
    int is_masked = (buffer[1] & WS_MASK_BIT) != 0;
    int payload_len_tmp = buffer[1] & 0x7F;
    int header_len = 2;
    
    if (payload_len_tmp == 126) {
        if (len < 4) return -1;
        payload_len_tmp = (buffer[2] << 8) | buffer[3];
        header_len = 4;
    } else if (payload_len_tmp == 127) {
        if (len < 10) return -1;
        payload_len_tmp = 0;
        for (int i = 0; i < 8; i++) {
            payload_len_tmp = (payload_len_tmp << 8) | (unsigned char)buffer[2 + i];
        }
        header_len = 10;
    }
    
    int mask_offset = header_len;
    int data_offset = header_len + (is_masked ? 4 : 0);
    
    if (len < data_offset + payload_len_tmp) return -1;
    
    *payload_len = payload_len_tmp;
    unsigned char mask[4] = {0};
    
    if (is_masked) {
        memcpy(mask, buffer + mask_offset, 4);
        for (int i = 0; i < payload_len_tmp; i++) {
            payload[i] = buffer[data_offset + i] ^ mask[i % 4];
        }
    } else {
        memcpy(payload, buffer + data_offset, payload_len_tmp);
    }
    
    return data_offset + payload_len_tmp;
}

int tcp_connect(const char* host, int port) {
    struct hostent* server = gethostbyname(host);
    if (server == NULL) {
        fprintf(stderr, "Cannot resolve host: %s\n", host);
        return -1;
    }
    
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    memcpy(&addr.sin_addr.s_addr, server->h_addr, server->h_length);
    addr.sin_port = htons(port);
    
    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sockfd);
        return -1;
    }
    
    return sockfd;
}

// WebSocket handshake with Bearer token (following nullclaw requirements)
int websocket_handshake(ws_client_t* client, const char* host, int port, const char* path) {
    char key[64];
    generate_websocket_key(key, sizeof(key));
    
    // Build request exactly as nullclaw expects
    char request[4096];
    int len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Origin: http://localhost:42617\r\n"
        "Authorization: Bearer %s\r\n"
        "X-NullClaw-Agent: %s\r\n"
        "X-WebChannel-Version: 1\r\n"
        "\r\n",
        path, host, port, key, client->auth_token, client->agent_id);
    
    printf("=== Handshake Request ===\n%s\n=== End Request ===\n", request);
    
    if (send(client->sockfd, request, len, 0) < 0) {
        perror("send");
        return -1;
    }
    
    char response[4096];
    int recv_len = recv(client->sockfd, response, sizeof(response) - 1, 0);
    if (recv_len <= 0) {
        perror("recv");
        return -1;
    }
    response[recv_len] = '\0';
    
    printf("=== Handshake Response ===\n%s\n=== End Response ===\n", response);
    
    // Check for 101 Switching Protocols
    if (strstr(response, "101") == NULL) {
        fprintf(stderr, "Handshake failed\n");
        return -1;
    }
    
    // Verify accept key
    char* server_key_start = strstr(response, "Sec-WebSocket-Accept:");
    if (server_key_start) {
        server_key_start += 22;
        while (*server_key_start == ' ') server_key_start++;
        char* server_key_end = strstr(server_key_start, "\r\n");
        if (server_key_end) {
            char server_key[128];
            int key_len = server_key_end - server_key_start;
            strncpy(server_key, server_key_start, key_len);
            server_key[key_len] = '\0';
            
            char expected_key[128];
            compute_accept_key(key, expected_key, sizeof(expected_key));
            
            printf("Server key: %s\n", server_key);
            printf("Expected key: %s\n", expected_key);
            
            if (strcmp(server_key, expected_key) != 0) {
                fprintf(stderr, "WebSocket accept key mismatch\n");
                return -1;
            }
        }
    }
    
    return 0;
}

int ws_send_text(ws_client_t* client, const char* message) {
    char buffer[65536];
    int frame_len = create_ws_frame(buffer, WS_OPCODE_TEXT, message, strlen(message), 1);
    int ret = send(client->sockfd, buffer, frame_len, 0);
    if (ret < 0) {
        perror("ws_send_text");
    }
    return ret;
}

char* ws_recv_text(ws_client_t* client) {
    static char recv_buffer[65536];
    static char payload[65536];
    int total_len = 0;
    
    while (1) {
        int recv_len = recv(client->sockfd, recv_buffer + total_len, 
                            sizeof(recv_buffer) - total_len - 1, 0);
        if (recv_len <= 0) {
            if (recv_len == 0) {
                printf("Connection closed by peer\n");
            } else {
                perror("recv");
            }
            return NULL;
        }
        total_len += recv_len;
        
        int payload_len, opcode;
        int consumed = parse_ws_frame(recv_buffer, total_len, payload, &payload_len, &opcode);
        
        if (consumed > 0) {
            if (opcode == WS_OPCODE_TEXT) {
                payload[payload_len] = '\0';
                return strdup(payload);
            } else if (opcode == WS_OPCODE_PING) {
                char pong[2];
                create_ws_frame(pong, WS_OPCODE_PONG, NULL, 0, 1);
                send(client->sockfd, pong, 2, 0);
            } else if (opcode == WS_OPCODE_CLOSE) {
                return NULL;
            }
            
            if (consumed < total_len) {
                memmove(recv_buffer, recv_buffer + consumed, total_len - consumed);
                total_len -= consumed;
            } else {
                total_len = 0;
            }
        } else if (total_len >= (int)sizeof(recv_buffer) - 1) {
            fprintf(stderr, "WebSocket frame too large\n");
            return NULL;
        }
    }
}

// Send user message with auth_token in payload
int send_user_message(ws_client_t* client, const char* content, const char* sender_id) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "type", "user_message");
    cJSON_AddStringToObject(root, "session_id", client->session_id);
    
    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "auth_token", client->auth_token);
    cJSON_AddStringToObject(payload, "content", content);
    if (sender_id) {
        cJSON_AddStringToObject(payload, "sender_id", sender_id);
    }
    cJSON_AddItemToObject(root, "payload", payload);
    
    char* json_str = cJSON_PrintUnformatted(root);
    printf("Sending: %s\n", json_str);
    int ret = ws_send_text(client, json_str);
    
    free(json_str);
    cJSON_Delete(root);
    
    return ret;
}

void handle_incoming_message(ws_client_t* client, const char* message) {
    cJSON* root = cJSON_Parse(message);
    if (!root) {
        printf("Received (non-JSON): %s\n", message);
        return;
    }
    
    cJSON* type = cJSON_GetObjectItem(root, "type");
    if (!type || type->type != cJSON_String) {
        cJSON_Delete(root);
        return;
    }
    
    if (strcmp(type->valuestring, "assistant_chunk") == 0 ||
        strcmp(type->valuestring, "assistant_final") == 0) {
        cJSON* payload = cJSON_GetObjectItem(root, "payload");
        if (payload && payload->type == cJSON_Object) {
            cJSON* content = cJSON_GetObjectItem(payload, "content");
            if (content && content->type == cJSON_String) {
                printf("%s", content->valuestring);
                fflush(stdout);
                if (strcmp(type->valuestring, "assistant_final") == 0) {
                    printf("\n");
                }
            }
        }
    } else if (strcmp(type->valuestring, "error") == 0) {
        cJSON* payload = cJSON_GetObjectItem(root, "payload");
        if (payload && payload->type == cJSON_Object) {
            cJSON* err_msg = cJSON_GetObjectItem(payload, "message");
            if (err_msg && err_msg->type == cJSON_String) {
                fprintf(stderr, "\n❌ Error: %s\n", err_msg->valuestring);
            }
            cJSON* code = cJSON_GetObjectItem(payload, "code");
            if (code && code->type == cJSON_String) {
                fprintf(stderr, "   Code: %s\n", code->valuestring);
            }
        }
    } else {
        printf("Received: %s\n", message);
    }
    
    cJSON_Delete(root);
}

void run_client(ws_client_t* client) {
    printf("\n✅ Connected to WebSocket server\n");
    printf("Type your messages and press Enter to send.\n");
    printf("Type '/quit' to exit.\n\n");
    
    fd_set readfds;
    char input_buffer[4096];
    
    while (client->is_connected) {
        FD_ZERO(&readfds);
        FD_SET(client->sockfd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);
        
        int max_fd = client->sockfd > STDIN_FILENO ? client->sockfd : STDIN_FILENO;
        int activity = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        
        if (activity < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        
        if (FD_ISSET(client->sockfd, &readfds)) {
            char* message = ws_recv_text(client);
            if (message == NULL) {
                printf("\nConnection closed by server\n");
                break;
            }
            handle_incoming_message(client, message);
            free(message);
        }
        
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (fgets(input_buffer, sizeof(input_buffer), stdin) != NULL) {
                input_buffer[strcspn(input_buffer, "\n")] = '\0';
                if (strcmp(input_buffer, "/quit") == 0) {
                    break;
                }
                if (strlen(input_buffer) > 0) {
                    send_user_message(client, input_buffer, "c-client");
                }
            }
        }
    }
}

int main(int argc, char* argv[]) {
    const char* host = "localhost";
    int port = 42617;
    const char* path = "/ws/chat";
    const char* auth_token = NULL;
    const char* session_id = "c-session-1";
    
    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) host = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) path = argv[++i];
        else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) auth_token = argv[++i];
        else if (strcmp(argv[i], "--session") == 0 && i + 1 < argc) session_id = argv[++i];
    }
    
    if (auth_token == NULL) {
        fprintf(stderr, "Usage: %s --token <auth_token>\n", argv[0]);
        return 1;
    }
    
    // Initialize OpenSSL
    OpenSSL_add_all_algorithms();
    
    ws_client_t client;
    memset(&client, 0, sizeof(client));
    client.is_connected = 1;
    strncpy(client.auth_token, auth_token, sizeof(client.auth_token) - 1);
    strncpy(client.agent_id, "c-client", sizeof(client.agent_id) - 1);
    strncpy(client.session_id, session_id, sizeof(client.session_id) - 1);
    
    printf("Connecting to ws://%s:%d%s\n", host, port, path);
    printf("Auth token: %s\n", auth_token);
    printf("Session ID: %s\n", session_id);
    
    client.sockfd = tcp_connect(host, port);
    if (client.sockfd < 0) return 1;
    
    if (websocket_handshake(&client, host, port, path) < 0) {
        close(client.sockfd);
        return 1;
    }
    
    run_client(&client);
    
    close(client.sockfd);
    
    printf("\nDisconnected\n");
    return 0;
}