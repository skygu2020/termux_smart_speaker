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
static pthread_mutex_t g_synthesis_mutex = PTHREAD_MUTEX_INITIALIZER;

// ============= OpenSL ES 播放器 =============
typedef struct {
    SLObjectItf engineObject;
    SLEngineItf engineEngine;
    SLObjectItf outputMixObject;
    SLObjectItf playerObject;
    SLPlayItf playerPlay;
    SLBufferQueueItf playerBufferQueue;
    SLVolumeItf playerVolume;
    short **buffers;
    int buffer_count, buffer_size;
    int read_index, write_index, queued_count;
    int playing, should_stop, producer_done, playback_complete;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} StreamPlayer;

static StreamPlayer *g_player = NULL;

// OpenSL ES 每消费完一个缓冲区调用一次。
static void player_callback(SLBufferQueueItf caller, void *context) {
    (void)caller;
    StreamPlayer *p = (StreamPlayer *)context;
    pthread_mutex_lock(&p->mutex);
    if (!p->should_stop) {
        if (p->queued_count > 0) {
            p->queued_count--;
            p->read_index = (p->read_index + 1) % p->buffer_count;
        }
        if (p->producer_done && p->queued_count == 0) {
            p->playing = 0;
            p->playback_complete = 1;
        }
        pthread_cond_broadcast(&p->cond);
    }
    pthread_mutex_unlock(&p->mutex);
}

static StreamPlayer *stream_player_init(int sample_rate, int buffer_count) {
    SLresult r = SL_RESULT_SUCCESS;
    StreamPlayer *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->buffer_count = buffer_count;
    p->buffer_size = AUDIO_CHUNK_SIZE;
    pthread_mutex_init(&p->mutex, NULL);
    pthread_cond_init(&p->cond, NULL);
    p->buffers = calloc((size_t)buffer_count, sizeof(short *));
    if (!p->buffers) goto error;
    for (int i=0;i<buffer_count;i++) {
        p->buffers[i] = calloc((size_t)p->buffer_size, sizeof(short));
        if (!p->buffers[i]) goto error;
    }

    r = slCreateEngine(&p->engineObject,0,NULL,0,NULL,NULL);
    if (r != SL_RESULT_SUCCESS) goto error;
    r = (*p->engineObject)->Realize(p->engineObject,SL_BOOLEAN_FALSE);
    if (r != SL_RESULT_SUCCESS) goto error;
    r = (*p->engineObject)->GetInterface(p->engineObject,SL_IID_ENGINE,&p->engineEngine);
    if (r != SL_RESULT_SUCCESS) goto error;
    r = (*p->engineEngine)->CreateOutputMix(p->engineEngine,&p->outputMixObject,0,NULL,NULL);
    if (r != SL_RESULT_SUCCESS) goto error;
    r = (*p->outputMixObject)->Realize(p->outputMixObject,SL_BOOLEAN_FALSE);
    if (r != SL_RESULT_SUCCESS) goto error;

    SLDataLocator_AndroidSimpleBufferQueue loc = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,(SLuint32)buffer_count
    };
    SLDataFormat_PCM fmt = {
        SL_DATAFORMAT_PCM,1,(SLuint32)(sample_rate*1000),
        SL_PCMSAMPLEFORMAT_FIXED_16,SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_SPEAKER_FRONT_CENTER,SL_BYTEORDER_LITTLEENDIAN
    };
    SLDataSource src = {&loc,&fmt};
    SLDataLocator_OutputMix outloc = {SL_DATALOCATOR_OUTPUTMIX,p->outputMixObject};
    SLDataSink sink = {&outloc,NULL};
    const SLInterfaceID ids[] = {SL_IID_BUFFERQUEUE,SL_IID_VOLUME};
    const SLboolean req[] = {SL_BOOLEAN_TRUE,SL_BOOLEAN_TRUE};
    r = (*p->engineEngine)->CreateAudioPlayer(p->engineEngine,&p->playerObject,&src,&sink,2,ids,req);
    if (r != SL_RESULT_SUCCESS) goto error;
    r = (*p->playerObject)->Realize(p->playerObject,SL_BOOLEAN_FALSE);
    if (r != SL_RESULT_SUCCESS) goto error;
    r = (*p->playerObject)->GetInterface(p->playerObject,SL_IID_PLAY,&p->playerPlay);
    if (r != SL_RESULT_SUCCESS) goto error;
    r = (*p->playerObject)->GetInterface(p->playerObject,SL_IID_BUFFERQUEUE,&p->playerBufferQueue);
    if (r != SL_RESULT_SUCCESS) goto error;
    r = (*p->playerObject)->GetInterface(p->playerObject,SL_IID_VOLUME,&p->playerVolume);
    if (r != SL_RESULT_SUCCESS) goto error;
    r = (*p->playerBufferQueue)->RegisterCallback(p->playerBufferQueue,player_callback,p);
    if (r != SL_RESULT_SUCCESS) goto error;
    return p;

error:
    fprintf(stderr,"[TTS] 播放器初始化失败 (SLresult=%u)\n",(unsigned)r);
    if (p->playerObject) (*p->playerObject)->Destroy(p->playerObject);
    if (p->outputMixObject) (*p->outputMixObject)->Destroy(p->outputMixObject);
    if (p->engineObject) (*p->engineObject)->Destroy(p->engineObject);
    if (p->buffers) { for(int i=0;i<buffer_count;i++) free(p->buffers[i]); free(p->buffers); }
    pthread_mutex_destroy(&p->mutex);
    pthread_cond_destroy(&p->cond);
    free(p);
    return NULL;
}

// 按播放器块大小拆分 PCM；队列满时等待消费回调，形成有界背压。
static int stream_player_enqueue(StreamPlayer *p,const short *data,int n) {
    int off=0;
    while(off<n) {
        pthread_mutex_lock(&p->mutex);
        while(p->queued_count>=p->buffer_count && !p->should_stop)
            pthread_cond_wait(&p->cond,&p->mutex);
        if(p->should_stop || !p->playing || p->producer_done) {
            pthread_mutex_unlock(&p->mutex); return -1;
        }
        int count=n-off;
        if(count>p->buffer_size) count=p->buffer_size;
        int slot=p->write_index;
        memcpy(p->buffers[slot],data+off,(size_t)count*sizeof(short));
        SLresult r=(*p->playerBufferQueue)->Enqueue(
            p->playerBufferQueue,p->buffers[slot],(SLuint32)((size_t)count*sizeof(short)));
        if(r!=SL_RESULT_SUCCESS) {
            pthread_mutex_unlock(&p->mutex);
            fprintf(stderr,"[TTS] BufferQueue Enqueue 失败: %u\n",(unsigned)r);
            return -1;
        }
        p->write_index=(p->write_index+1)%p->buffer_count;
        p->queued_count++;
        off+=count;
        pthread_mutex_unlock(&p->mutex);
    }
    return 0;
}

// 仅在合成请求互斥锁保护下调用。
static int stream_player_start(StreamPlayer *p) {
    pthread_mutex_lock(&p->mutex);
    p->should_stop=1; p->playing=0;
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->mutex);
    (*p->playerPlay)->SetPlayState(p->playerPlay,SL_PLAYSTATE_STOPPED);
    (*p->playerBufferQueue)->Clear(p->playerBufferQueue);
    pthread_mutex_lock(&p->mutex);
    p->read_index=0; p->write_index=0; p->queued_count=0;
    p->playing=1; p->should_stop=0; p->producer_done=0; p->playback_complete=0;
    pthread_mutex_unlock(&p->mutex);
    SLresult r=(*p->playerPlay)->SetPlayState(p->playerPlay,SL_PLAYSTATE_PLAYING);
    if(r!=SL_RESULT_SUCCESS) {
        pthread_mutex_lock(&p->mutex);
        p->playing=0; p->should_stop=1;
        pthread_cond_broadcast(&p->cond);
        pthread_mutex_unlock(&p->mutex);
        fprintf(stderr,"[TTS] 设置播放状态失败: %u\n",(unsigned)r);
        return -1;
    }
    return 0;
}

static void stream_player_finish(StreamPlayer *p) {
    pthread_mutex_lock(&p->mutex);
    p->producer_done=1;
    if(p->queued_count==0) { p->playing=0; p->playback_complete=1; }
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->mutex);
}

static void stream_player_stop(StreamPlayer *p) {
    if(!p) return;
    pthread_mutex_lock(&p->mutex);
    p->playing=0; p->should_stop=1; p->playback_complete=1;
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->mutex);
    if(p->playerPlay) (*p->playerPlay)->SetPlayState(p->playerPlay,SL_PLAYSTATE_STOPPED);
    if(p->playerBufferQueue) (*p->playerBufferQueue)->Clear(p->playerBufferQueue);
}

static void stream_player_wait(StreamPlayer *p) {
    pthread_mutex_lock(&p->mutex);
    while(!p->playback_complete && !p->should_stop)
        pthread_cond_wait(&p->cond,&p->mutex);
    pthread_mutex_unlock(&p->mutex);
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

// ============= HTTP 请求处理 =============
typedef struct {
    StreamPlayer *player;
    int failed;
    int chunk_count;
} TtsProgressContext;

// 回调提供的是刚生成的 PCM；指针仅在本次回调期间有效。
static int32_t tts_progress_callback(const float *samples,int32_t n,float progress,void *arg) {
    (void)progress;
    TtsProgressContext *ctx=(TtsProgressContext *)arg;
    if(!ctx || !samples || n<=0) return 1;
    short pcm[AUDIO_CHUNK_SIZE];
    int off=0;
    while(off<n) {
        int count=n-off;
        if(count>AUDIO_CHUNK_SIZE) count=AUDIO_CHUNK_SIZE;
        for(int i=0;i<count;i++) {
            float v=samples[off+i];
            if(v>1.0f) v=1.0f;
            if(v< -1.0f) v= -1.0f;
            pcm[i]=(short)(v*32767.0f);
        }
        if(stream_player_enqueue(ctx->player,pcm,count)!=0) {
            ctx->failed=1;
            return 0;
        }
        off+=count;
    }
    ctx->chunk_count++;
    return 1;
}

static void handle_synthesize(const char *text,float speed,int client_fd,int is_stream) {
    pthread_mutex_lock(&g_synthesis_mutex);
    printf("[TTS] 开始增量合成: %s\n",text);

    if(stream_player_start(g_player)!=0) {
        if(client_fd>0) { shutdown(client_fd,SHUT_RDWR); close(client_fd); }
        pthread_mutex_unlock(&g_synthesis_mutex);
        return;
    }

    SherpaOnnxGenerationConfig cfg;
    memset(&cfg,0,sizeof(cfg));
    cfg.sid=0;
    cfg.speed=speed;
    cfg.silence_scale=0.2f;

    TtsProgressContext ctx={.player=g_player,.failed=0,.chunk_count=0};
    const SherpaOnnxGeneratedAudio *audio=SherpaOnnxOfflineTtsGenerateWithConfig(
        g_tts,text,&cfg,tts_progress_callback,&ctx);

    if(!audio || audio->n<=0 || ctx.failed) {
        fprintf(stderr,"[TTS] 增量合成失败%s\n",ctx.failed?"（播放队列入队失败）":"");
        stream_player_stop(g_player);
    } else {
        printf("[TTS] 合成完成: %d samples, %.2f 秒, 回调片段 %d 个\n",
               audio->n,(float)audio->n/16000.0f,ctx.chunk_count);
        stream_player_finish(g_player);
        printf("[TTS] 等待播放队列排空...\n");
        stream_player_wait(g_player);
        printf("[TTS] 播放完成\n");
    }

    if(audio) SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);

    if(client_fd>0) {
        if(is_stream) {
            const char *msg=(ctx.failed || !audio)?"data: [ERROR]\n\n":"data: [DONE]\n\n";
            write(client_fd,msg,strlen(msg));
        }
        shutdown(client_fd,SHUT_RDWR);
        close(client_fd);
    }
    pthread_mutex_unlock(&g_synthesis_mutex);
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
    
    const char *input_text = cJSON_IsString(input) ? input->valuestring : "";
    char *text = strdup(input_text);
    float speed_val = cJSON_IsNumber(speed) ? (float)speed->valuedouble : 1.0f;

    cJSON_Delete(json);
    if (!text) {
        const char *response = "HTTP/1.1 500 Internal Server Error\\r\\nContent-Length: 0\\r\\nConnection: close\\r\\n\\r\\n";
        write(client_fd, response, strlen(response));
        close(client_fd);
        return NULL;
    }

    printf("[HTTP] 收到: %s\\n", text);

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
    free(text);
    
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