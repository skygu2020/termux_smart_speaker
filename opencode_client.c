#define _POSIX_C_SOURCE 200809L
#include "opencode_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

#define OPENCODE_CLI "opencode2"
#define OPENCODE_AGENT "build"
#define OPENCODE_SESSION_TITLE "Termux 语音助手"
#define OPENCODE_TIMEOUT_SECONDS 600L
#define RESPONSE_LIMIT (4U * 1024U * 1024U)

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} Buffer;

static char g_base_url[128];
static char g_session_id[160];
static char *g_password;
static int g_initialized;

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userdata) {
    Buffer *b = (Buffer *)userdata;
    size_t n = size * nmemb;
    if (n > RESPONSE_LIMIT || b->size > RESPONSE_LIMIT - n) return 0;
    if (b->size + n + 1 > b->capacity) {
        size_t cap = b->capacity ? b->capacity : 1024;
        while (cap < b->size + n + 1) cap *= 2;
        char *p = (char *)realloc(b->data, cap);
        if (!p) return 0;
        b->data = p;
        b->capacity = cap;
    }
    memcpy(b->data + b->size, contents, n);
    b->size += n;
    b->data[b->size] = '\0';
    return n;
}

/* Run a fixed argv directly (no shell), capture stdout, and avoid logging secrets. */
static char *capture_command(char *const argv[]) {
    int fds[2];
    pid_t pid;
    Buffer out = {0};
    char chunk[512];
    ssize_t n;
    int status = 0;

    if (pipe(fds) != 0) return NULL;
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    close(fds[1]);
    while ((n = read(fds[0], chunk, sizeof(chunk))) > 0) {
        if ((size_t)n > RESPONSE_LIMIT || out.size > RESPONSE_LIMIT - (size_t)n) {
            free(out.data);
            out.data = NULL;
            break;
        }
        if (out.size + (size_t)n + 1 > out.capacity) {
            size_t cap = out.capacity ? out.capacity : 512;
            while (cap < out.size + (size_t)n + 1) cap *= 2;
            char *p = (char *)realloc(out.data, cap);
            if (!p) {
                free(out.data);
                out.data = NULL;
                break;
            }
            out.data = p;
            out.capacity = cap;
        }
        memcpy(out.data + out.size, chunk, (size_t)n);
        out.size += (size_t)n;
        out.data[out.size] = '\0';
    }
    close(fds[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || !out.data) {
        free(out.data);
        return NULL;
    }
    if (!out.data) {
        out.data = (char *)calloc(1, 1);
    }
    return out.data;
}

static void trim(char *s) {
    size_t n;
    char *p;
    if (!s) return;
    p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

/*
 * The service registration is the same file used by OpenCode's own client
 * discovery. It contains the advertised URL and, when enabled, the private
 * service password. Reading it avoids relying on shell aliases or parsing
 * human-readable CLI output.
 */
static int discover_from_registration(void) {
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_STATE_HOME");
    char path[1200], *contents = NULL;
    FILE *fp;
    long length;
    cJSON *json, *url_item, *password_item;
    char *colon;
    int port;

    if (xdg && *xdg) {
        if (snprintf(path, sizeof(path), "%s/opencode/service.json", xdg) >= (int)sizeof(path))
            return -1;
    } else {
        if (!home || !*home) return -1;
        if (snprintf(path, sizeof(path), "%s/.local/state/opencode/service.json", home) >= (int)sizeof(path))
            return -1;
    }

    fp = fopen(path, "rb");
    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_END) != 0 || (length = ftell(fp)) <= 0 || length > 65536 ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    contents = (char *)malloc((size_t)length + 1);
    if (!contents) {
        fclose(fp);
        return -1;
    }
    if (fread(contents, 1, (size_t)length, fp) != (size_t)length) {
        fclose(fp);
        free(contents);
        return -1;
    }
    fclose(fp);
    contents[length] = '\0';

    json = cJSON_Parse(contents);
    free(contents);
    if (!json) return -1;

    url_item = cJSON_GetObjectItemCaseSensitive(json, "url");
    password_item = cJSON_GetObjectItemCaseSensitive(json, "password");
    if (!url_item || !cJSON_IsString(url_item) || !url_item->valuestring) {
        cJSON_Delete(json);
        return -1;
    }

    colon = strrchr(url_item->valuestring, ':');
    if (!colon || (port = atoi(colon + 1)) < 1 || port > 65535) {
        cJSON_Delete(json);
        return -1;
    }
    snprintf(g_base_url, sizeof(g_base_url), "http://127.0.0.1:%d", port);

    if (password_item && cJSON_IsString(password_item) &&
        password_item->valuestring && *password_item->valuestring) {
        g_password = strdup(password_item->valuestring);
    }
    cJSON_Delete(json);
    return 0;
}

static int discover_service(void) {
    if (discover_from_registration() == 0) return 0;

    /* Fallback for non-default XDG/channel layouts. */
    {
        char *argv_status[] = { (char *)OPENCODE_CLI, "service", "status", NULL };
        char *status = capture_command(argv_status);
        char *colon;
        int port;

        if (!status || !*status) {
            fprintf(stderr, "[OpenCode] 无法读取服务注册文件，也无法执行 opencode2 service status\n");
            free(status);
            return -1;
        }
        trim(status);
        colon = strrchr(status, ':');
        if (!colon || (port = atoi(colon + 1)) < 1 || port > 65535) {
            fprintf(stderr, "[OpenCode] 无法解析服务地址: %s\n", status);
            free(status);
            return -1;
        }
        snprintf(g_base_url, sizeof(g_base_url), "http://127.0.0.1:%d", port);
        free(status);
    }

    {
        char *argv_password[] = { (char *)OPENCODE_CLI, "service", "get", "password", NULL };
        char *password = capture_command(argv_password);
        if (password) {
            trim(password);
            if (*password) g_password = password;
            else free(password);
        }
    }
    return 0;
}

static int api_request(const char *method, const char *path, const char *body,
                       char **response, long *http_status) {
    CURL *curl;
    CURLcode rc;
    struct curl_slist *headers = NULL;
    Buffer out = {0};
    char url[768];

    if (response) *response = NULL;
    if (http_status) *http_status = 0;
    if (!g_base_url[0]) return -1;

    snprintf(url, sizeof(url), "%s%s", g_base_url, path);
    curl = curl_easy_init();
    if (!curl) return -1;

    headers = curl_slist_append(headers, "Accept: application/json");
    if (body) headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, OPENCODE_TIMEOUT_SECONDS);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    if (g_password && *g_password) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(curl, CURLOPT_USERNAME, "opencode");
        curl_easy_setopt(curl, CURLOPT_PASSWORD, g_password);
    }

    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_status);
        if (!out.data) out.data = strdup("");
        if (response) *response = out.data;
        else free(out.data);
    } else {
        fprintf(stderr, "[OpenCode] HTTP 请求失败: %s (%s)\n",
                curl_easy_strerror(rc), path);
        free(out.data);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK ? 0 : -1;
}

static int mkdir_tree(const char *path) {
    char tmp[1024];
    size_t i, n;
    if (!path || !(n = strlen(path)) || n >= sizeof(tmp)) return -1;
    memcpy(tmp, path, n + 1);
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int session_state_path(char *out, size_t out_size) {
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;
    if (snprintf(out, out_size, "%s/.local/state/termux_smart_speaker", home) >= (int)out_size)
        return -1;
    return 0;
}

static int save_session_id(const char *id) {
    char dir[1024], path[1200];
    int fd;
    if (session_state_path(dir, sizeof(dir)) != 0 || mkdir_tree(dir) != 0) return -1;
    if (snprintf(path, sizeof(path), "%s/opencode_session", dir) >= (int)sizeof(path)) return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    if (write(fd, id, strlen(id)) != (ssize_t)strlen(id) || write(fd, "\n", 1) != 1) {
        close(fd);
        return -1;
    }
    close(fd);
    chmod(path, 0600);
    return 0;
}

static int load_session_id(char *id, size_t size) {
    char dir[1024], path[1200];
    FILE *fp;
    size_t i;
    if (session_state_path(dir, sizeof(dir)) != 0) return -1;
    if (snprintf(path, sizeof(path), "%s/opencode_session", dir) >= (int)sizeof(path)) return -1;
    fp = fopen(path, "r");
    if (!fp) return -1;
    if (!fgets(id, (int)size, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    trim(id);
    if (!*id) return -1;
    for (i = 0; id[i]; i++) {
        if (!(isalnum((unsigned char)id[i]) || id[i] == '_' || id[i] == '-')) return -1;
    }
    return 0;
}

static int validate_session(const char *id) {
    char path[256], *response = NULL;
    long status = 0;
    int ok;
    snprintf(path, sizeof(path), "/api/session/%s", id);
    if (api_request("GET", path, NULL, &response, &status) != 0) return 0;
    ok = status == 200;
    free(response);
    return ok;
}

static int create_session(void) {
    const char *home = getenv("HOME");
    char directory[1024];
    cJSON *root, *location;
    char *body, *response = NULL;
    cJSON *json, *data, *id;
    long status = 0;
    int rc = -1;

    if (!home || !*home) return -1;
    if (snprintf(directory, sizeof(directory), "%s/opencode/termux_smart_speaker_new", home)
        >= (int)sizeof(directory)) return -1;

    root = cJSON_CreateObject();
    location = cJSON_CreateObject();
    if (!root || !location) {
        cJSON_Delete(root);
        cJSON_Delete(location);
        return -1;
    }
    cJSON_AddStringToObject(root, "title", OPENCODE_SESSION_TITLE);
    cJSON_AddStringToObject(root, "agent", OPENCODE_AGENT);
    cJSON_AddStringToObject(location, "directory", directory);
    cJSON_AddItemToObject(root, "location", location);
    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return -1;

    if (api_request("POST", "/api/session", body, &response, &status) == 0 &&
        status == 200 && response) {
        json = cJSON_Parse(response);
        data = json ? cJSON_GetObjectItemCaseSensitive(json, "data") : NULL;
        id = data ? cJSON_GetObjectItemCaseSensitive(data, "id") : NULL;
        if (id && cJSON_IsString(id) && id->valuestring && *id->valuestring &&
            strlen(id->valuestring) < sizeof(g_session_id)) {
            snprintf(g_session_id, sizeof(g_session_id), "%s", id->valuestring);
            save_session_id(g_session_id);
            rc = 0;
        }
        cJSON_Delete(json);
    } else {
        fprintf(stderr, "[OpenCode] 创建会话失败，HTTP %ld: %s\n",
                status, response ? response : "");
    }

    free(body);
    free(response);
    return rc;
}

int opencode_init(void) {
    char *info = NULL;
    long status = 0;

    if (g_initialized) return 0;
    if (discover_service() != 0) return -1;

    if (api_request("GET", "/api/info", NULL, &info, &status) != 0 || status != 200) {
        fprintf(stderr, "[OpenCode] API 健康检查失败，HTTP %ld\n", status);
        free(info);
        return -1;
    }
    free(info);

    if (load_session_id(g_session_id, sizeof(g_session_id)) != 0 ||
        !validate_session(g_session_id)) {
        g_session_id[0] = '\0';
        if (create_session() != 0) return -1;
    }

    g_initialized = 1;
    printf("[OpenCode] 已连接 %s，会话 %s\n", g_base_url, g_session_id);
    return 0;
}

static int wait_for_session(void) {
    char path[256], *response = NULL;
    long status = 0;
    int i;

    snprintf(path, sizeof(path), "/api/experimental/session/%s/wait", g_session_id);
    /* This endpoint has no request body in OpenCode v2.4.2. */
    if (api_request("POST", path, NULL, &response, &status) == 0 && status == 204) {
        free(response);
        return 0;
    }
    free(response);

    /* Fallback for builds where the experimental wait route is unavailable. */
    snprintf(path, sizeof(path), "/api/session/active");
    for (i = 0; i < 600; i++) {
        cJSON *json, *data, *entry;
        int active = 0;
        response = NULL;
        status = 0;
        if (api_request("GET", path, NULL, &response, &status) != 0 || status != 200) {
            free(response);
            sleep(1);
            continue;
        }
        json = cJSON_Parse(response);
        data = json ? cJSON_GetObjectItemCaseSensitive(json, "data") : NULL;
        /*
         * OpenCode v2.4.2 returns active sessions as an object keyed by
         * session ID, not as an array of objects.
         */
        entry = cJSON_IsObject(data)
            ? cJSON_GetObjectItemCaseSensitive(data, g_session_id)
            : NULL;
        if (entry) active = 1;
        cJSON_Delete(json);
        free(response);
        if (!active) return 0;
        sleep(1);
    }
    fprintf(stderr, "[OpenCode] 等待 Agent 执行超时\n");
    return -1;
}

static char *read_latest_assistant_text(char **message_id_out) {
    char path[256], *response = NULL, *result = NULL;
    long status = 0;
    cJSON *json, *data;
    int i, count;

    if (message_id_out) *message_id_out = NULL;
    snprintf(path, sizeof(path), "/api/session/%s/message?limit=30&order=desc", g_session_id);
    if (api_request("GET", path, NULL, &response, &status) != 0 || status != 200) {
        fprintf(stderr, "[OpenCode] 读取会话消息失败，HTTP %ld\n", status);
        free(response);
        return NULL;
    }

    json = cJSON_Parse(response);
    free(response);
    data = json ? cJSON_GetObjectItemCaseSensitive(json, "data") : NULL;
    if (!cJSON_IsArray(data) && cJSON_IsArray(json)) data = json;
    if (!cJSON_IsArray(data)) {
        cJSON_Delete(json);
        return NULL;
    }

    count = cJSON_GetArraySize(data);
    for (i = 0; i < count; i++) {
        cJSON *entry = cJSON_GetArrayItem(data, i);
        cJSON *type = cJSON_GetObjectItemCaseSensitive(entry, "type");
        cJSON *content = cJSON_GetObjectItemCaseSensitive(entry, "content");
        cJSON *id = cJSON_GetObjectItemCaseSensitive(entry, "id");
        int j, part_count;
        size_t used = 0;
        char *joined = (char *)calloc(1, 1);

        if (!type || !cJSON_IsString(type) || strcmp(type->valuestring, "assistant") != 0 ||
            !cJSON_IsArray(content)) {
            free(joined);
            continue;
        }

        part_count = cJSON_GetArraySize(content);
        for (j = 0; j < part_count; j++) {
            cJSON *part = cJSON_GetArrayItem(content, j);
            cJSON *part_type = cJSON_GetObjectItemCaseSensitive(part, "type");
            cJSON *text = cJSON_GetObjectItemCaseSensitive(part, "text");
            if (part_type && cJSON_IsString(part_type) &&
                strcmp(part_type->valuestring, "text") == 0 &&
                text && cJSON_IsString(text) && text->valuestring) {
                size_t len = strlen(text->valuestring);
                char *next = (char *)realloc(joined, used + len + 2);
                if (!next) {
                    free(joined);
                    joined = NULL;
                    break;
                }
                joined = next;
                if (used) joined[used++] = '\n';
                memcpy(joined + used, text->valuestring, len);
                used += len;
                joined[used] = '\0';
            }
        }
        if (joined && used) {
            result = joined;
            if (message_id_out && id && cJSON_IsString(id) &&
                id->valuestring && *id->valuestring) {
                *message_id_out = strdup(id->valuestring);
            }
            break;
        }
        free(joined);
    }

    cJSON_Delete(json);
    return result;
}

int opencode_send_message(const char *message, OpenCodeReply *reply) {
    cJSON *root;
    char *body, *response = NULL, *answer, *message_id = NULL;
    char path[256];
    long status = 0;

    if (!reply) return -1;
    memset(reply, 0, sizeof(*reply));
    if (!message || !*message) return -1;
    if (!g_initialized && opencode_init() != 0) return -1;

    root = cJSON_CreateObject();
    if (!root) return -1;
    cJSON_AddStringToObject(root, "text", message);
    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return -1;

    snprintf(path, sizeof(path), "/api/session/%s/prompt", g_session_id);
    if (api_request("POST", path, body, &response, &status) != 0 || status != 200) {
        fprintf(stderr, "[OpenCode] 提交语音消息失败，HTTP %ld: %s\n",
                status, response ? response : "");
        free(body);
        free(response);
        return -1;
    }
    free(body);
    free(response);

    if (wait_for_session() != 0) return -1;
    answer = read_latest_assistant_text(&message_id);
    if (!answer || !*answer) {
        free(answer);
        free(message_id);
        fprintf(stderr, "[OpenCode] Agent 已结束，但没有可播报的文本回复\n");
        return -1;
    }

    reply->text = answer;
    reply->session_id = strdup(g_session_id);
    reply->message_id = message_id;
    if (!reply->session_id) {
        opencode_reply_free(reply);
        return -1;
    }
    if (!reply->message_id) {
        fprintf(stderr, "[OpenCode] 警告：回复缺少 message_id，本次 TTS 无法参与跨客户端去重\n");
    }
    return 0;
}

void opencode_reply_free(OpenCodeReply *reply) {
    if (!reply) return;
    free(reply->text);
    free(reply->session_id);
    free(reply->message_id);
    memset(reply, 0, sizeof(*reply));
}

void opencode_close(void) {
    free(g_password);
    g_password = NULL;
    g_initialized = 0;
    g_base_url[0] = '\0';
    g_session_id[0] = '\0';
}
