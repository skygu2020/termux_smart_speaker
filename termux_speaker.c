#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include "sherpa-onnx/c-api/c-api.h"
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include "opencode_client.h"

// ============= 配置 =============
#define SAMPLE_RATE 16000
#define BUFFER_SIZE 512
#define VAD_WINDOW_SIZE 512
#define MAX_SEGMENT_DURATION 10
#define TTS_URL "http://127.0.0.1:8080/v1/audio/speech"


// ============= 唤醒随机回复语 =============
static const char* wake_responses[] = {
    "我在",
    "嗯？",
    "在呢",
    "请说",
    "来了",
    "听着呢",
    "什么事",
    "请讲",
    "您好",
    "请说指令"
};
static const int wake_responses_count = sizeof(wake_responses) / sizeof(wake_responses[0]);

// 获取随机回复
static const char* get_random_wake_response(void) {
    int index = rand() % wake_responses_count;
    return wake_responses[index];
}

typedef enum {
    STATE_IDLE = 0,      // 空闲，等待唤醒词
    STATE_WAKEUP = 1,    // 已唤醒，等待用户命令
    STATE_TTS = 2        // TTS 播放中
} SystemState;

static volatile int g_system_state = STATE_IDLE;
static volatile int g_endpoint_processing = 0;
static int init_wake_word(void);
static void rebuild_kws(void);

// ============= 全局变量 =============
static volatile int g_running = 1;

static SherpaOnnxOnlineRecognizer *g_recognizer = NULL;
static SherpaOnnxVoiceActivityDetector *g_vad = NULL;
static const SherpaOnnxDisplay *g_display = NULL;

static const SherpaOnnxKeywordSpotter *g_kws = NULL;
static const SherpaOnnxOnlineStream *g_kws_stream = NULL;
static SherpaOnnxOnlineStream *g_stream = NULL;
static int g_wake_word_detected = 0;

// OpenSL ES 录音相关
static SLObjectItf g_engineObject = NULL;
static SLEngineItf g_engineEngine = NULL;
static SLObjectItf g_recorderObject = NULL;
static SLRecordItf g_recorderRecord = NULL;
static SLAndroidSimpleBufferQueueItf g_recorderBufferQueue = NULL;
static short g_recorderBuffer[BUFFER_SIZE];

// OpenSLESRecorder 结构体定义
typedef struct {
    SLObjectItf engineObject;
    SLEngineItf engineEngine;
    SLObjectItf recorderObject;
    SLRecordItf recorderRecord;
    SLAndroidSimpleBufferQueueItf recorderBuffQueueItf;
    SLAndroidConfigurationItf configItf;
    FILE* pfile;
    int16_t* buffer;
    size_t bufferSize;
} OpenSLESRecorder;

// ============= 音频数据队列 =============
typedef struct AudioQueue {
    float* data;
    int size;
    struct AudioQueue* next;
} AudioQueue;

static AudioQueue* g_queue_head = NULL;
static AudioQueue* g_queue_tail = NULL;
static pthread_mutex_t g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_queue_cond = PTHREAD_COND_INITIALIZER;

static void clear_audio_queue(void) {
    pthread_mutex_lock(&g_queue_mutex);
    while (g_queue_head != NULL) {
        AudioQueue* node = g_queue_head;
        g_queue_head = node->next;
        free(node->data);
        free(node);
    }
    g_queue_tail = NULL;
    pthread_mutex_unlock(&g_queue_mutex);
}

// ============= CURL 响应回调 =============
typedef struct {
    char* data;
    size_t size;
} HttpResponse;

static size_t http_write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    HttpResponse* resp = (HttpResponse*)userp;
    
    char* new_data = realloc(resp->data, resp->size + total_size + 1);
    if (!new_data) return 0;
    
    resp->data = new_data;
    memcpy(resp->data + resp->size, contents, total_size);
    resp->size += total_size;
    resp->data[resp->size] = '\0';
    
    return total_size;
}

// 检查是否为 Emoji 的 UTF-8 编码
static int is_emoji_start(const unsigned char* s) {
    // 4字节emoji (F0 9F ...)
    if (s[0] == 0xF0 && s[1] == 0x9F) {
        // 表情符号范围 0x80-0xBF
        if (s[2] >= 0x80 && s[2] <= 0xBF) {
            return 4;
        }
        // 特定表情
        if (s[2] == 0x98 || s[2] == 0x99 || s[2] == 0x8E || s[2] == 0x8F) {
            return 4;
        }
    }
    // 4字节emoji (F0 9F A4 ...) 如 🤔
    if (s[0] == 0xF0 && s[1] == 0x9F && s[2] == 0xA4) {
        return 4;
    }
    // 4字节emoji (F0 9F A5 ...) 如 🥰
    if (s[0] == 0xF0 && s[1] == 0x9F && s[2] == 0xA5) {
        return 4;
    }
    // 4字节emoji (F0 9F A6 ...) 如 🦞 (龙虾)
    if (s[0] == 0xF0 && s[1] == 0x9F && s[2] == 0xA6) {
        return 4;
    }
    // 4字节emoji (F0 9F A7 ...) 如 🧠
    if (s[0] == 0xF0 && s[1] == 0x9F && s[2] == 0xA7) {
        return 4;
    }
    // 4字节emoji (F0 9F A9 ...)
    if (s[0] == 0xF0 && s[1] == 0x9F && s[2] == 0xA9) {
        return 4;
    }
    // 3字节emoji (E2 80 ...)
    if (s[0] == 0xE2 && s[1] == 0x80 && s[2] >= 0xA0 && s[2] <= 0xBF) {
        return 3;
    }
    // 特殊符号 (E2 9D ...)
    if (s[0] == 0xE2 && s[1] == 0x9D && s[2] >= 0xA4 && s[2] <= 0xA7) {
        return 3;
    }
    // 星形等 (E2 98 ...)
    if (s[0] == 0xE2 && s[1] == 0x98 && s[2] == 0x85) {
        return 3;
    }
    // 箭头等 (E2 96 ...)
    if (s[0] == 0xE2 && s[1] == 0x96 && s[2] >= 0x80 && s[2] <= 0xBF) {
        return 3;
    }
    return 0;
}

static char* clean_markdown(const char* text) {
    if (!text) return NULL;
    
    char* result = strdup(text);
    if (!result) return NULL;
    
    char* src = result;
    char* dst = result;
    
    while (*src) {
        // 检查是否为 Emoji
        int emoji_len = is_emoji_start((const unsigned char*)src);
        if (emoji_len > 0) {
            src += emoji_len;
            continue;
        }
        
        // 处理 UTF-8 多字节字符（中文标点）
        if ((unsigned char)*src >= 0xE2) {
            // 中文双引号 “ ”
            if ((unsigned char)*src == 0xE2 && (unsigned char)*(src+1) == 0x80 && 
                ((unsigned char)*(src+2) == 0x9C || (unsigned char)*(src+2) == 0x9D)) {
                *dst++ = ' ';
                src += 3;
                continue;
            }
            // 中文单引号 ‘ ’
            if ((unsigned char)*src == 0xE2 && (unsigned char)*(src+1) == 0x80 && 
                ((unsigned char)*(src+2) == 0x98 || (unsigned char)*(src+2) == 0x99)) {
                *dst++ = ' ';
                src += 3;
                continue;
            }
            // 破折号 —
            if ((unsigned char)*src == 0xE2 && (unsigned char)*(src+1) == 0x80 && 
                (unsigned char)*(src+2) == 0x94) {
                *dst++ = ' ';
                src += 3;
                continue;
            }
            // 省略号 …
            if ((unsigned char)*src == 0xE2 && (unsigned char)*(src+1) == 0x80 && 
                (unsigned char)*(src+2) == 0xA6) {
                *dst++ = ' ';
                src += 3;
                continue;
            }
            // 其他 UTF-8 字符，保留
            *dst++ = *src++;
            continue;
        }
        
        // 处理 ASCII 字符
        switch (*src) {
            case '*':
            case '_':
            case '`':
            case '#':
            case '[':
            case ']':
            case '(':
            case ')':
            case '{':
            case '}':
            case '|':
            case '>':
            case '<':
            case '~':
            case '\\':
                src++;
                continue;
            case '"':    // 英文半角双引号 -> 替换为空格
            case '\'':   // 英文半角单引号 -> 替换为空格
                *dst++ = ' ';
                src++;
                continue;
            case ':':    // 冒号
            case ';':    // 分号
                *dst++ = ' ';
                src++;
                continue;
            default:
                *dst++ = *src++;
                break;
        }
    }
    
    *dst = '\0';
    
    // 合并多个空格
    src = result;
    dst = result;
    int last_space = 0;
    while (*src) {
        if (*src == ' ') {
            if (!last_space && dst != result) {
                *dst++ = ' ';
                last_space = 1;
            }
        } else {
            *dst++ = *src;
            last_space = 0;
        }
        src++;
    }
    *dst = '\0';
    
    // 去除首尾空格
    while (*result == ' ') {
        memmove(result, result + 1, strlen(result));
    }
    char* end = result + strlen(result) - 1;
    while (end > result && *end == ' ') {
        *end = '\0';
        end--;
    }
    
    return result;
}

// ============= HTTP 请求封装 =============
static char* http_post(const char* url, const char* json_body) {
    CURL* curl = curl_easy_init();
    if (!curl) return NULL;
    
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    HttpResponse resp = {0};
    resp.data = malloc(1);
    resp.data[0] = '\0';
    resp.size = 0;
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    
    CURLcode res = curl_easy_perform(curl);
    
    char* response_text = NULL;
    if (res == CURLE_OK && resp.size > 0) {
        response_text = resp.data;
    } else {
        free(resp.data);
    }
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    return response_text;
}

static void rebuild_kws(void) {
    printf("[KWS] 重建唤醒词检测器...\n");
    
    // 销毁旧的
    if (g_kws_stream) {
        SherpaOnnxDestroyOnlineStream(g_kws_stream);
        g_kws_stream = NULL;
    }
    if (g_kws) {
        SherpaOnnxDestroyKeywordSpotter(g_kws);
        g_kws = NULL;
    }
    
    // 重新初始化
    init_wake_word();
    
    printf("[KWS] 重建完成\n");
}

// ============= 调用 TTS（带调用编号诊断）=============
static volatile unsigned long g_tts_call_sequence = 0;

static void call_tts_with_ids(const char* text, const char* session_id, const char* message_id) {
    if (!text || strlen(text) == 0) return;

    // 原子递增：即使未来从多个线程调用，也能区分每一次调用。
    unsigned long call_id = __sync_add_and_fetch(&g_tts_call_sequence, 1);
    printf("\n[TTS-CALL #%lu] call_tts() 被调用，文本长度=%zu，内容=%s\n",
           call_id, strlen(text), text);
    if (session_id && *session_id && message_id && *message_id) {
        printf("[TTS-CALL #%lu] OpenCode identity session=%s message=%s\n",
               call_id, session_id, message_id);
    }
    fflush(stdout);

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        fprintf(stderr, "[TTS-CALL #%lu] 创建 JSON 对象失败\n", call_id);
        return;
    }
    cJSON_AddStringToObject(root, "input", text);
    cJSON_AddStringToObject(root, "voice", "default");
    cJSON_AddNumberToObject(root, "speed", 1.0);
    if (session_id && *session_id) cJSON_AddStringToObject(root, "session_id", session_id);
    if (message_id && *message_id) cJSON_AddStringToObject(root, "message_id", message_id);

    char* json_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_body) {
        fprintf(stderr, "[TTS-CALL #%lu] JSON 序列化失败，未发送 HTTP 请求\n", call_id);
        return;
    }

    printf("[TTS-CALL #%lu] 开始发送 HTTP POST -> %s\n", call_id, TTS_URL);
    fflush(stdout);

    char* response = http_post(TTS_URL, json_body);

    printf("[TTS-CALL #%lu] HTTP POST 已返回，结果=%s\n",
           call_id, response ? "收到响应正文" : "无响应正文或请求失败");
    fflush(stdout);

    // 等待尾音消失
    usleep(300000);
    clear_audio_queue();

    free(json_body);

    if (response) {
        free(response);
    }
}


static void call_tts(const char* text) {
    call_tts_with_ids(text, NULL, NULL);
}

// ============= 语音播报（设置播放标志）=============
static void speak_text_with_ids(const char* text, const char* session_id, const char* message_id) {
    if (!text || strlen(text) == 0) return;
    
    printf("[TTS] 开始播放: %s\n", text);
    call_tts_with_ids(text, session_id, message_id);
    printf("[TTS] 播放完成\n");
    
    //usleep(300000);
    clear_audio_queue();
    
    // ========== 关键：重建 KWS ==========
    rebuild_kws();
    
}

// 统一的发送函数
// ============= 处理识别结果 =============
static void process_recognized_text(const char* text) {
    if (!text || strlen(text) == 0) return;

    OpenCodeReply reply = {0};
    if (opencode_send_message(text, &reply) == 0 && reply.text && *reply.text) {
        char* clean_response = clean_markdown(reply.text);
        if (clean_response && *clean_response) {
            speak_text_with_ids(clean_response, reply.session_id, reply.message_id);
        }
        free(clean_response);
        opencode_reply_free(&reply);
    } else {
        opencode_reply_free(&reply);
        call_tts("抱歉，我没有理解你的问题");
    }
}

/*
static void process_recognized_text(const char* text) {
    if (!text || strlen(text) == 0) return;
    
    printf("[LLM] 发送: %s\n", text);
    
    char* response = call_zero(text);
    
    if (response && strlen(response) > 0) {
        printf("[LLM] 回复: %s\n", response);
        speak_text(response);
        free(response);
    } else {
        printf("[LLM] 无响应，尝试非流式\n");
        response = call_llm(text);
        if (response && strlen(response) > 0) {
            printf("[LLM] 回复: %s\n", response);
            speak_text(response);
            free(response);
        } else {
            speak_text("抱歉，我没有理解你的问题");
        }
    }
}
*/
// ============= 入队/出队 =============
static void enqueue_audio(const float* samples, int num_samples) {
    AudioQueue* node = (AudioQueue*)malloc(sizeof(AudioQueue));
    node->data = (float*)malloc(num_samples * sizeof(float));
    memcpy(node->data, samples, num_samples * sizeof(float));
    node->size = num_samples;
    node->next = NULL;
    
    pthread_mutex_lock(&g_queue_mutex);
    if (g_queue_tail) {
        g_queue_tail->next = node;
        g_queue_tail = node;
    } else {
        g_queue_head = g_queue_tail = node;
    }
    pthread_cond_signal(&g_queue_cond);
    pthread_mutex_unlock(&g_queue_mutex);
}

static AudioQueue* dequeue_audio(void) {
    pthread_mutex_lock(&g_queue_mutex);
    while (g_queue_head == NULL && g_running) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&g_queue_cond, &g_queue_mutex, &ts);
    }
    
    AudioQueue* node = g_queue_head;
    if (node) {
        g_queue_head = node->next;
        if (g_queue_head == NULL) {
            g_queue_tail = NULL;
        }
    }
    pthread_mutex_unlock(&g_queue_mutex);
    return node;
}

// ============= OpenSL ES 录音回调（检查播放状态）=============
static void recorder_callback(SLAndroidSimpleBufferQueueItf bq, void* context) {
    
    // 正常识别流程
    float float_buffer[BUFFER_SIZE];
    for (int i = 0; i < BUFFER_SIZE; i++) {
        float_buffer[i] = g_recorderBuffer[i] / 32768.0f;
    }
    
    // ========== 唤醒词检测 ==========
    switch (g_system_state) {
        case STATE_IDLE:
            if (!g_wake_word_detected && g_kws && g_kws_stream) {
                SherpaOnnxOnlineStreamAcceptWaveform(g_kws_stream, SAMPLE_RATE, float_buffer, BUFFER_SIZE);
                
                if (SherpaOnnxIsKeywordStreamReady(g_kws, g_kws_stream)) {
                    SherpaOnnxDecodeKeywordStream(g_kws, g_kws_stream);
                }
                
                const SherpaOnnxKeywordResult *result = SherpaOnnxGetKeywordResult(g_kws, g_kws_stream);
                if (result && result->keyword && strlen(result->keyword) > 0) {
                    printf("\n🎯 检测到唤醒词: %s\n", result->keyword);
                    g_system_state = STATE_WAKEUP;
                    g_wake_word_detected = 1;
                    printf("\a");
                    call_tts(get_random_wake_response());
                    
                    SherpaOnnxResetKeywordStream(g_kws, g_kws_stream);
                }
                SherpaOnnxDestroyKeywordResult(result);
            }
            break;
            
        
        case STATE_WAKEUP:
            // ========== 唤醒后识别 ==========
            if (g_wake_word_detected) {
                // 添加静态变量来保存识别结果
                static char last_text[1024] = {0};
                
                // 直接进行 ASR 识别
                SherpaOnnxOnlineStreamAcceptWaveform(g_stream, SAMPLE_RATE, float_buffer, BUFFER_SIZE);
                
                while (SherpaOnnxIsOnlineStreamReady(g_recognizer, g_stream)) {
                    SherpaOnnxDecodeOnlineStream(g_recognizer, g_stream);
                }
                
                const SherpaOnnxOnlineRecognizerResult* result = 
                    SherpaOnnxGetOnlineStreamResult(g_recognizer, g_stream);
                
                if (result->text && strlen(result->text) > 0) {
                    if (strcmp(last_text, result->text) != 0) {
                        strcpy(last_text, result->text);
                        printf("\r🎙️ 识别中: %-60s", result->text);
                        fflush(stdout);
                    }
                }
                
                // 端点检测：先锁定本轮结果并切换状态，再执行阻塞的 OpenCode/TTS。
                // 防止端点回调重入时重复提交同一段识别文本。
                if (SherpaOnnxOnlineStreamIsEndpoint(g_recognizer, g_stream)) {
                    if (last_text[0] != '\0' &&
                        __sync_lock_test_and_set(&g_endpoint_processing, 1) == 0) {
                        char final_text[sizeof(last_text)];
                        snprintf(final_text, sizeof(final_text), "%s", last_text);
                        g_system_state = STATE_TTS;
                        g_wake_word_detected = 0;
                        SherpaOnnxOnlineStreamReset(g_recognizer, g_stream);
                        last_text[0] = '\0';

                        printf("\n📝 识别结果: %s\n", final_text);
                        process_recognized_text(final_text);

                        g_system_state = STATE_IDLE;
                        __sync_lock_release(&g_endpoint_processing);
                    } else {
                        SherpaOnnxOnlineStreamReset(g_recognizer, g_stream);
                        last_text[0] = '\0';
                    }
                }
                
                SherpaOnnxDestroyOnlineRecognizerResult(result);
            }
            break;
            
        case STATE_TTS:
            // TTS 播放中，什么都不做，只保持录音循环
            break;
    }
    
    if (g_running) {
        (*g_recorderBufferQueue)->Enqueue(g_recorderBufferQueue, 
                                               g_recorderBuffer, 
                                               BUFFER_SIZE * sizeof(short));
    }
}

// ============= 初始化 OpenSL ES 录音 =============
static int init_recorder(void) {
    SLresult result;
    
    result = slCreateEngine(&g_engineObject, 0, NULL, 0, NULL, NULL);
    if (result != SL_RESULT_SUCCESS) return -1;
    (*g_engineObject)->Realize(g_engineObject, SL_BOOLEAN_FALSE);
    (*g_engineObject)->GetInterface(g_engineObject, SL_IID_ENGINE, &g_engineEngine);
    
    (*g_engineEngine)->CreateOutputMix(g_engineEngine, &g_engineObject, 0, NULL, NULL);
    
    SLDataLocator_IODevice loc_dev = {
        SL_DATALOCATOR_IODEVICE,
        SL_IODEVICE_AUDIOINPUT,
        SL_DEFAULTDEVICEID_AUDIOINPUT,
        NULL
    };
    SLDataSource audioSrc = {&loc_dev, NULL};
    
    SLDataLocator_AndroidSimpleBufferQueue loc_bq = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2
    };
    SLDataFormat_PCM format_pcm = {
        SL_DATAFORMAT_PCM, 1, SL_SAMPLINGRATE_16,
        SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN
    };
    SLDataSink audioSnk = {&loc_bq, &format_pcm};
    
    const SLInterfaceID id[1] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean req[1] = {SL_BOOLEAN_TRUE};
    
    result = (*g_engineEngine)->CreateAudioRecorder(g_engineEngine, &g_recorderObject,
                                                     &audioSrc, &audioSnk, 1, id, req);
    if (result != SL_RESULT_SUCCESS) return -1;
    
    (*g_recorderObject)->Realize(g_recorderObject, SL_BOOLEAN_FALSE);
    (*g_recorderObject)->GetInterface(g_recorderObject, SL_IID_RECORD, &g_recorderRecord);
    (*g_recorderObject)->GetInterface(g_recorderObject, SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                       &g_recorderBufferQueue);
    (*g_recorderBufferQueue)->RegisterCallback(g_recorderBufferQueue, recorder_callback, NULL);
    
    return 0;
}

static void start_recording(void) {
    (*g_recorderBufferQueue)->Enqueue(g_recorderBufferQueue, g_recorderBuffer, 
                                       BUFFER_SIZE * sizeof(short));
    (*g_recorderRecord)->SetRecordState(g_recorderRecord, SL_RECORDSTATE_RECORDING);
    printf("🎤 开始录音...\n");
}

static void stop_recording(void) {
    if (g_recorderRecord) {
        (*g_recorderRecord)->SetRecordState(g_recorderRecord, SL_RECORDSTATE_STOPPED);
    }
    if (g_recorderObject) {
        (*g_recorderObject)->Destroy(g_recorderObject);
        g_recorderObject = NULL;
    }
    if (g_engineObject) {
        (*g_engineObject)->Destroy(g_engineObject);
        g_engineObject = NULL;
    }
}


static int init_wake_word(void) {
    SherpaOnnxKeywordSpotterConfig config;
    memset(&config, 0, sizeof(config));
    
    config.model_config.transducer.encoder = "./sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01/encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx";
    config.model_config.transducer.decoder = "./sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01/decoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx";
    config.model_config.transducer.joiner = "./sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01/joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx";
    config.model_config.tokens = "./sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01/tokens.txt";
    config.model_config.num_threads = 2;
    config.model_config.provider = "cpu";
    config.model_config.debug = 0;
    
    config.feat_config.sample_rate = 16000;
    config.feat_config.feature_dim = 80;
    
    // KWS 特定配置
    config.max_active_paths = 4;
    config.num_trailing_blanks = 1;
    config.keywords_score = 1.0f;
    config.keywords_threshold = 0.25f;
    config.keywords_file = "./sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01/keywords.txt";
    
    g_kws = SherpaOnnxCreateKeywordSpotter(&config);
    if (!g_kws) {
        printf("⚠️ 唤醒词初始化失败\n");
        return -1;
    }
    
    // 创建 KWS 流
    g_kws_stream = SherpaOnnxCreateKeywordStream(g_kws);
    if (!g_kws_stream) {
        printf("⚠️ KWS 流创建失败\n");
        SherpaOnnxDestroyKeywordSpotter(g_kws);
        g_kws = NULL;
        return -1;
    }
    
    printf("✅ 唤醒词检测器初始化成功\n");
    return 0;
}

// ============= 初始化 ASR =============
static int init_asr(void) {
    SherpaOnnxOnlineRecognizerConfig config;
    memset(&config, 0, sizeof(config));
    
    config.feat_config.sample_rate = SAMPLE_RATE;
    config.feat_config.feature_dim = 80;
    config.model_config.num_threads = 2;
    config.model_config.provider = "cpu";
    config.model_config.debug = 0;
    //config.model_config.tokens = "./bilingual-zh-en-2023-02-20/tokens.txt";
    //config.model_config.transducer.encoder = "./bilingual-zh-en-2023-02-20/encoder-epoch-99-avg-1.int8.onnx";
    //config.model_config.transducer.decoder = "./bilingual-zh-en-2023-02-20/decoder-epoch-99-avg-1.onnx";
    //config.model_config.transducer.joiner = "./bilingual-zh-en-2023-02-20/joiner-epoch-99-avg-1.int8.onnx";
    
    config.model_config.tokens = "./sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30/tokens.txt";
    config.model_config.transducer.encoder = "./sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30/encoder.int8.onnx";
    config.model_config.transducer.decoder = "./sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30/decoder.onnx";
    config.model_config.transducer.joiner = "./sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30/joiner.int8.onnx";
    
    
    config.decoding_method = "greedy_search";
    config.max_active_paths = 4;
    config.enable_endpoint = 1;
    config.rule1_min_trailing_silence = 2.4;
    config.rule2_min_trailing_silence = 1.2;
    config.rule3_min_utterance_length = 300;
    
    g_recognizer = (SherpaOnnxOnlineRecognizer*)SherpaOnnxCreateOnlineRecognizer(&config);
    if (!g_recognizer) return -1;
    
    // 创建 ASR 流
    g_stream = (SherpaOnnxOnlineStream*)SherpaOnnxCreateOnlineStream(g_recognizer);
    if (!g_stream) {
        printf("❌ ASR 流创建失败\n");
        return -1;
    }
    
    g_display = SherpaOnnxCreateDisplay(50);
    printf("✅ ASR 初始化成功\n");
    return 0;
}

// ============= 初始化 VAD =============
static int init_vad(void) {
    SherpaOnnxVadModelConfig vad_config;
    memset(&vad_config, 0, sizeof(vad_config));
    
    vad_config.silero_vad.model = "./silero_vad.onnx";
    vad_config.silero_vad.threshold = 0.5;
    vad_config.silero_vad.min_silence_duration = 0.5;
    vad_config.silero_vad.min_speech_duration = 0.25;
    vad_config.silero_vad.max_speech_duration = MAX_SEGMENT_DURATION;
    vad_config.silero_vad.window_size = VAD_WINDOW_SIZE;
    vad_config.sample_rate = SAMPLE_RATE;
    vad_config.num_threads = 1;
    vad_config.debug = 0;
    
    g_vad = (SherpaOnnxVoiceActivityDetector*)SherpaOnnxCreateVoiceActivityDetector(&vad_config, 30);
    if (!g_vad) return -1;
    
    printf("✅ VAD 初始化成功\n");
    return 0;
}

// ============= VAD + ASR 处理线程 =============
static void* vad_asr_thread(void* arg) {
    SherpaOnnxOnlineStream* stream = (SherpaOnnxOnlineStream*)SherpaOnnxCreateOnlineStream(g_recognizer);
    char accumulated_text[4096] = {0};
    char last_result[4096] = {0};
    int in_speech = 0;
    int silence_frames = 0;
    
    while (g_running) {
        
        AudioQueue* node = dequeue_audio();
        if (!node) continue;
        
        SherpaOnnxVoiceActivityDetectorAcceptWaveform(g_vad, node->data, node->size);
        SherpaOnnxOnlineStreamAcceptWaveform(stream, SAMPLE_RATE, node->data, node->size);
        
        while (SherpaOnnxIsOnlineStreamReady(g_recognizer, stream)) {
            SherpaOnnxDecodeOnlineStream(g_recognizer, stream);
        }
        
        const SherpaOnnxOnlineRecognizerResult* result = 
            SherpaOnnxGetOnlineStreamResult(g_recognizer, stream);
        
        if (result->text && strlen(result->text) > 0) {
            if (strcmp(last_result, result->text) != 0) {
                strcpy(last_result, result->text);
                printf("\r🎙️ 识别中: %-60s", result->text);
                fflush(stdout);
            }
        }
        SherpaOnnxDestroyOnlineRecognizerResult(result);
        
        int has_speech = 0;
        while (!SherpaOnnxVoiceActivityDetectorEmpty(g_vad)) {
            const SherpaOnnxSpeechSegment* segment = 
                SherpaOnnxVoiceActivityDetectorFront(g_vad);
            if (segment && segment->n > 0) {
                has_speech = 1;
                if (!in_speech) {
                    in_speech = 1;
                    silence_frames = 0;
                    printf("\n🎙️ 语音开始\n");
                }
            }
            SherpaOnnxDestroySpeechSegment(segment);
            SherpaOnnxVoiceActivityDetectorPop(g_vad);
        }
        
        if (in_speech && !has_speech) {
            silence_frames++;
        } else {
            silence_frames = 0;
        }
        
        int is_endpoint = SherpaOnnxOnlineStreamIsEndpoint(g_recognizer, stream);
        
        if (in_speech && (silence_frames > 10 || is_endpoint)) {
            printf("\n🔇 语音结束\n");
            
            usleep(300000);
            
            while (SherpaOnnxIsOnlineStreamReady(g_recognizer, stream)) {
                SherpaOnnxDecodeOnlineStream(g_recognizer, stream);
            }
            
            const SherpaOnnxOnlineRecognizerResult* final_result = 
                SherpaOnnxGetOnlineStreamResult(g_recognizer, stream);
            
            if (final_result->text && strlen(final_result->text) > 0) {
                strcpy(accumulated_text, final_result->text);
                printf("📝 识别结果: %s\n", accumulated_text);
                
                if (strlen(accumulated_text) > 0) {
                    process_recognized_text(accumulated_text);
                }
            }
            SherpaOnnxDestroyOnlineRecognizerResult(final_result);
            
            SherpaOnnxOnlineStreamReset(g_recognizer, stream);
            accumulated_text[0] = '\0';
            last_result[0] = '\0';
            in_speech = 0;
            silence_frames = 0;
        }
        
        free(node->data);
        free(node);
    }
    
    SherpaOnnxDestroyOnlineStream(stream);
    return NULL;
}

// ============= 信号处理 =============
static void signal_handler(int sig) {
    printf("\n🛑 正在退出...\n");
    g_running = 0;
    //exit(0);
    pthread_cond_signal(&g_queue_cond);
}

// ============= 主函数 =============
int main() {
    signal(SIGINT, signal_handler);
    
    printf("🚀 VAD + ASR + LLM 语音助手启动\n");
    printf("================================\n");
    
    curl_global_init(CURL_GLOBAL_ALL);
    

    if (opencode_init() != 0) {
        printf("⚠️ OpenCode 初始化失败；语音识别仍可运行，发送消息时会重试\n");
    } else {
        printf("✅ OpenCode 语音会话已就绪\n");
    }
    if (init_wake_word() != 0) {
        printf("⚠️ 唤醒词功能未启用，将一直处于唤醒状态\n");
        g_wake_word_detected = 1;  // 降级为一直识别
    }
    
    if (init_asr() != 0) {
        fprintf(stderr, "❌ ASR 初始化失败\n");
        return 1;
    }
    
    if (init_vad() != 0) {
        fprintf(stderr, "❌ VAD 初始化失败\n");
        return 1;
    }
    
    if (init_recorder() != 0) {
        fprintf(stderr, "❌ 录音初始化失败\n");
        return 1;
    }
    
    start_recording();
    
    printf("✅ 系统就绪，请说话...\n");
    printf("================================\n");
    
    while (g_running) {
        sleep(1);
    }
    
    stop_recording();
    g_running = 0;
    opencode_close();
    
    if (g_vad) SherpaOnnxDestroyVoiceActivityDetector(g_vad);
    if (g_recognizer) SherpaOnnxDestroyOnlineRecognizer(g_recognizer);
    if (g_display) SherpaOnnxDestroyDisplay(g_display);
    if (g_kws_stream) SherpaOnnxDestroyOnlineStream(g_kws_stream);
    if (g_stream) SherpaOnnxDestroyOnlineStream(g_stream);
    if (g_kws) SherpaOnnxDestroyKeywordSpotter(g_kws);
    
    
    curl_global_cleanup();
    
    printf("👋 程序退出\n");
    return 0;
}
