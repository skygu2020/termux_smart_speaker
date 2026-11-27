#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include "sherpa-onnx/c-api/c-api.h"
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

// ============= 配置 =============
#define PORT 8080
#define BUFFER_SIZE 4096
#define AUDIO_CHUNK_SIZE 4096

// ============= 全局变量 =============
static volatile int g_running = 1;
static const SherpaOnnxOfflineTts *g_tts = NULL;

// ============= OpenSL ES 播放器 =============
typedef struct {
    SLObjectItf engineObject;
    SLEngineItf engineEngine;
    SLObjectItf outputMixObject;
    SLObjectItf playerObject;
    SLPlayItf playerPlay;
    SLBufferQueueItf playerBufferQueue;
    SLVolumeItf playerVolume;
    
    short** buffers;
    int buffer_count;
    int buffer_size;
    int current_buffer;
    volatile int playing;
    volatile int has_data;
    volatile int should_stop;
    volatile int playback_complete;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} StreamPlayer;

static StreamPlayer *g_player = NULL;

// 回调函数
static void player_callback(SLBufferQueueItf caller, void* context) {
    StreamPlayer* player = (StreamPlayer*)context;
    
    pthread_mutex_lock(&player->mutex);
    
    if (player->should_stop) {
        pthread_mutex_unlock(&player->mutex);
        return;
    }
    
    // 检查是否还有数据需要播放
    if (!player->has_data) {
        // 没有更多数据，停止播放
        player->playing = 0;
        player->playback_complete = 1;
        pthread_cond_signal(&player->cond);
        pthread_mutex_unlock(&player->mutex);
        return;
    }
    
    player->current_buffer = (player->current_buffer + 1) % player->buffer_count;
    player->has_data = 0;
    pthread_cond_signal(&player->cond);
    
    pthread_mutex_unlock(&player->mutex);
}


// 初始化播放器
static StreamPlayer* stream_player_init(int sample_rate, int buffer_count) {
    SLresult result;
    StreamPlayer* player = (StreamPlayer*)calloc(1, sizeof(StreamPlayer));
    if (!player) return NULL;
    
    player->buffer_count = buffer_count;
    player->buffer_size = AUDIO_CHUNK_SIZE;
    player->current_buffer = 0;
    player->playing = 0;
    player->has_data = 0;
    player->should_stop = 0;
    player->playback_complete = 0;
    pthread_mutex_init(&player->mutex, NULL);
    pthread_cond_init(&player->cond, NULL);
    
    player->buffers = (short**)malloc(buffer_count * sizeof(short*));
    for (int i = 0; i < buffer_count; i++) {
        player->buffers[i] = (short*)malloc(player->buffer_size * sizeof(short));
        memset(player->buffers[i], 0, player->buffer_size * sizeof(short));
    }
    
    result = slCreateEngine(&player->engineObject, 0, NULL, 0, NULL, NULL);
    if (result != SL_RESULT_SUCCESS) goto error;
    (*player->engineObject)->Realize(player->engineObject, SL_BOOLEAN_FALSE);
    (*player->engineObject)->GetInterface(player->engineObject, SL_IID_ENGINE, &player->engineEngine);
    
    (*player->engineEngine)->CreateOutputMix(player->engineEngine, &player->outputMixObject, 0, NULL, NULL);
    (*player->outputMixObject)->Realize(player->outputMixObject, SL_BOOLEAN_FALSE);
    
    SLDataLocator_AndroidSimpleBufferQueue loc_bq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, buffer_count};
    SLDataFormat_PCM format_pcm = {
        SL_DATAFORMAT_PCM,
        1,
        sample_rate * 1000,
        SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_SPEAKER_FRONT_CENTER,
        SL_BYTEORDER_LITTLEENDIAN
    };
    
    SLDataSource audioSrc = {&loc_bq, &format_pcm};
    SLDataLocator_OutputMix loc_outmix = {SL_DATALOCATOR_OUTPUTMIX, player->outputMixObject};
    SLDataSink audioSnk = {&loc_outmix, NULL};
    
    const SLInterfaceID ids[] = {SL_IID_BUFFERQUEUE, SL_IID_VOLUME};
    const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
    
    (*player->engineEngine)->CreateAudioPlayer(player->engineEngine, &player->playerObject,
                                               &audioSrc, &audioSnk, 2, ids, req);
    (*player->playerObject)->Realize(player->playerObject, SL_BOOLEAN_FALSE);
    (*player->playerObject)->GetInterface(player->playerObject, SL_IID_PLAY, &player->playerPlay);
    (*player->playerObject)->GetInterface(player->playerObject, SL_IID_BUFFERQUEUE, &player->playerBufferQueue);
    (*player->playerObject)->GetInterface(player->playerObject, SL_IID_VOLUME, &player->playerVolume);
    
    (*player->playerBufferQueue)->RegisterCallback(player->playerBufferQueue, player_callback, player);
    
    return player;
    
error:
    fprintf(stderr, "播放器初始化失败\n");
    return NULL;
}

// 入队音频数据
static void stream_player_enqueue(StreamPlayer* player, short* data, int num_samples) {
    pthread_mutex_lock(&player->mutex);
    
    while (player->has_data && player->playing && !player->should_stop) {
        pthread_cond_wait(&player->cond, &player->mutex);
    }
    
    if (!player->playing || player->should_stop) {
        pthread_mutex_unlock(&player->mutex);
        return;
    }
    
    int copy_size = num_samples > player->buffer_size ? player->buffer_size : num_samples;
    memcpy(player->buffers[player->current_buffer], data, copy_size * sizeof(short));
    
    (*player->playerBufferQueue)->Enqueue(player->playerBufferQueue, 
                                           player->buffers[player->current_buffer], 
                                           copy_size * sizeof(short));
    player->has_data = 1;
    
    pthread_mutex_unlock(&player->mutex);
}

// 开始播放
static void stream_player_start(StreamPlayer* player) {
    pthread_mutex_lock(&player->mutex);
    player->playing = 1;
    player->should_stop = 0;
    player->has_data = 0;
    player->playback_complete = 0;
    pthread_mutex_unlock(&player->mutex);
    (*player->playerPlay)->SetPlayState(player->playerPlay, SL_PLAYSTATE_PLAYING);
}

// 停止播放
static void stream_player_stop(StreamPlayer* player) {
    pthread_mutex_lock(&player->mutex);
    player->playing = 0;
    player->should_stop = 1;
    pthread_cond_signal(&player->cond);
    pthread_mutex_unlock(&player->mutex);
    
    (*player->playerPlay)->SetPlayState(player->playerPlay, SL_PLAYSTATE_STOPPED);
    (*player->playerBufferQueue)->Clear(player->playerBufferQueue);
}

// 等待播放完成
static void stream_player_wait(StreamPlayer* player) {
    pthread_mutex_lock(&player->mutex);
    while (player->playing && !player->should_stop && !player->playback_complete) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&player->cond, &player->mutex, &ts);
    }
    pthread_mutex_unlock(&player->mutex);
}

// 销毁播放器
static void stream_player_destroy(StreamPlayer* player) {
    if (!player) return;
    stream_player_stop(player);
    if (player->playerObject) (*player->playerObject)->Destroy(player->playerObject);
    if (player->outputMixObject) (*player->outputMixObject)->Destroy(player->outputMixObject);
    if (player->engineObject) (*player->engineObject)->Destroy(player->engineObject);
    for (int i = 0; i < player->buffer_count; i++) {
        if (player->buffers[i]) free(player->buffers[i]);
    }
    free(player->buffers);
    pthread_mutex_destroy(&player->mutex);
    pthread_cond_destroy(&player->cond);
    free(player);
}

// ============= TTS 回调 =============
typedef struct {
    StreamPlayer* player;
    int client_fd;
    int is_stream;
    int chunk_count;
} TtsCallbackData;

// 音频回调
static void tts_callback(const float* samples, int n, void* user_data) {
    TtsCallbackData* data = (TtsCallbackData*)user_data;
    if (!data->player->playing) return;
    
    // 转换 float 到 short
    short pcm[n];
    for (int i = 0; i < n; i++) {
        float s = samples[i];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        pcm[i] = (short)(s * 32767.0f);
    }
    
    // 流式输出到 HTTP
    if (data->is_stream && data->client_fd > 0) {
        char chunk_data[256];
        snprintf(chunk_data, sizeof(chunk_data), "data: chunk_%d\n\n", data->chunk_count++);
        write(data->client_fd, chunk_data, strlen(chunk_data));
    }
    
    // 播放
    stream_player_enqueue(data->player, pcm, n);
}

// ============= HTTP 请求处理 =============
static void handle_synthesize(const char* text, float speed, int client_fd, int is_stream) {
    printf("[TTS] 开始合成: %s\n", text);
    
    // 开始播放
    stream_player_start(g_player);
    
    // 生成音频
    const SherpaOnnxGeneratedAudio *audio = SherpaOnnxOfflineTtsGenerate(
        g_tts, text, 0, speed);
    
    if (!audio || audio->n <= 0) {
        printf("[TTS] 合成失败\n");
        if (client_fd > 0) {
            const char* error_msg = "data: [ERROR]\n\n";
            write(client_fd, error_msg, strlen(error_msg));
            close(client_fd);
        }
        return;
    }
    
    printf("[TTS] 合成成功: %d 采样, %.2f 秒\n", audio->n, (float)audio->n / 16000.0f);
    
    // 分块发送并播放
    int chunk_size = AUDIO_CHUNK_SIZE;
    int offset = 0;
    int chunk_id = 0;
    
    while (offset < audio->n && !g_player->should_stop) {
        int remaining = audio->n - offset;
        int send_size = remaining > chunk_size ? chunk_size : remaining;
        
        // 转换 float 到 short
        short pcm[send_size];
        for (int i = 0; i < send_size; i++) {
            float s = audio->samples[offset + i];
            if (s > 1.0f) s = 1.0f;
            if (s < -1.0f) s = -1.0f;
            pcm[i] = (short)(s * 32767.0f);
        }
        
        // 流式输出到 HTTP（可选）
        if (is_stream && client_fd > 0) {
            char chunk_data[256];
            snprintf(chunk_data, sizeof(chunk_data), "data: chunk_%d\n\n", chunk_id++);
            write(client_fd, chunk_data, strlen(chunk_data));
        }
        
        // 播放
        stream_player_enqueue(g_player, pcm, send_size);
        offset += send_size;
        
        // 让出 CPU，避免阻塞
        usleep(10000);
    }
    
    // 标记没有更多数据了
    pthread_mutex_lock(&g_player->mutex);
    g_player->has_data = 0;
    pthread_mutex_unlock(&g_player->mutex);
    
    // 等待播放完成
    printf("[TTS] 等待播放完成...\n");
    stream_player_wait(g_player);
    printf("[TTS] 播放完成\n");
    
    // 发送完成标记并关闭连接
    if (client_fd > 0) {
        if (is_stream) {
            const char* done_msg = "data: [DONE]\n\n";
            write(client_fd, done_msg, strlen(done_msg));
        }
        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
        printf("[HTTP] 连接已关闭\n");
    }
    
    SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
}

// ============= HTTP 服务器 =============
static void* handle_client(void* arg) {
    int client_fd = *(int*)arg;
    free(arg);
    
    // 设置 socket 超时
    struct timeval tv;
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    char buffer[8192];
    int n = read(client_fd, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        close(client_fd);
        return NULL;
    }
    buffer[n] = '\0';
    
    char method[16], path[256];
    sscanf(buffer, "%s %s", method, path);
    
    if (strcmp(method, "POST") != 0 || strcmp(path, "/v1/audio/speech") != 0) {
        const char* response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        write(client_fd, response, strlen(response));
        close(client_fd);
        return NULL;
    }
    
    char* body = strstr(buffer, "\r\n\r\n");
    if (!body) {
        close(client_fd);
        return NULL;
    }
    body += 4;
    
    cJSON* json = cJSON_Parse(body);
    if (!json) {
        const char* response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        write(client_fd, response, strlen(response));
        close(client_fd);
        return NULL;
    }
    
    cJSON* input = cJSON_GetObjectItem(json, "input");
    cJSON* speed = cJSON_GetObjectItem(json, "speed");
    
    const char* text = input ? input->valuestring : "";
    float speed_val = speed ? (float)speed->valuedouble : 1.0f;
    
    cJSON_Delete(json);
    
    printf("[HTTP] 收到: %s\n", text);
    
    int is_stream = (strstr(buffer, "text/event-stream") != NULL);
    
    // 发送响应头
    if (is_stream) {
        const char* response = 
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n";
        write(client_fd, response, strlen(response));
    } else {
        const char* response = 
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Connection: close\r\n"
            "\r\n";
        write(client_fd, response, strlen(response));
    }
    
    // 处理合成
    handle_synthesize(text, speed_val, client_fd, is_stream);
    
    return NULL;
}

// ============= 信号处理 =============
static void signal_handler(int sig) {
    printf("\n正在关闭服务器...\n");
    g_running = 0;
    if (g_player) {
        stream_player_stop(g_player);
    }
    exit(0);
}

// ============= 主函数 =============
int main() {
    signal(SIGINT, signal_handler);
    
    printf("TTS Server 启动中...\n");
    
    // 初始化 TTS
    SherpaOnnxOfflineTtsConfig config;
    memset(&config, 0, sizeof(config));
    
    config.model.matcha.acoustic_model = "./matcha-icefall-zh-en/model-steps-3.onnx";
    config.model.matcha.vocoder = "./vocos-16khz-univ.onnx";
    config.model.matcha.lexicon = "./matcha-icefall-zh-en/lexicon.txt";
    config.model.matcha.tokens = "./matcha-icefall-zh-en/tokens.txt";
    config.model.matcha.data_dir = "./matcha-icefall-zh-en/espeak-ng-data";
    config.model.num_threads = 2;
    config.model.debug = 0;
    config.rule_fsts = "./matcha-icefall-zh-en/phone-zh.fst,./matcha-icefall-zh-en/date-zh.fst,./matcha-icefall-zh-en/number-zh.fst";
    
    g_tts = SherpaOnnxCreateOfflineTts(&config);
    if (!g_tts) {
        fprintf(stderr, "创建 TTS 失败\n");
        return 1;
    }
    
    // 初始化播放器
    g_player = stream_player_init(16000, 4);
    if (!g_player) {
        fprintf(stderr, "创建播放器失败\n");
        SherpaOnnxDestroyOfflineTts(g_tts);
        return 1;
    }
    
    // 启动 HTTP 服务器
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    
    if (listen(server_fd, 5) < 0) {
        perror("listen");
        return 1;
    }
    
    printf("TTS Server 运行在 http://localhost:%d\n", PORT);
    printf("OpenAI 兼容端点: POST /v1/audio/speech\n");
    printf("  {\n");
    printf("    \"input\": \"你好，世界\",\n");
    printf("    \"speed\": 1.0\n");
    printf("  }\n");
    
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) continue;
        
        int* pclient = malloc(sizeof(int));
        *pclient = client_fd;
        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, pclient);
        pthread_detach(tid);
    }
    
    close(server_fd);
    stream_player_destroy(g_player);
    SherpaOnnxDestroyOfflineTts(g_tts);
    
    return 0;
}