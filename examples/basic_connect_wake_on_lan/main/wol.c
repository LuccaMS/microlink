#include "wol.h"
#include "wol_targets.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "wol";

#define WOL_SHARED_TOKEN ""

#define WOL_VERIFY_GRACE_MS     5000
#define WOL_VERIFY_RETRY_MS     3000
#define WOL_VERIFY_TIMEOUT_MS   120000
#define WOL_VERIFY_CONNECT_TIMEOUT_MS 2000

typedef enum {
    WOL_VERIFY_NONE = 0,
    WOL_VERIFY_PENDING,
    WOL_VERIFY_SUCCESS,
    WOL_VERIFY_TIMEOUT,
} wol_verify_state_t;

typedef struct {
    wol_verify_state_t state;
    uint32_t started_at_ms;
    uint32_t finished_at_ms;
} wol_status_t;

static wol_status_t s_status[WOL_TARGET_COUNT];
static SemaphoreHandle_t s_status_mutex = NULL;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static int find_target(const char *name)
{
    for (size_t i = 0; i < WOL_TARGET_COUNT; i++) {
        if (strcmp(WOL_TARGETS[i].name, name) == 0) {
            return (int)i;
        }
    }
    /* Modo "so 1 PC": se nao bateu o nome mas so existe 1 alvo
       configurado, usa ele mesmo assim (evita erro por
       espaco/maiuscula/nome diferente entre front-end e firmware). */
    if (WOL_TARGET_COUNT == 1) {
        ESP_LOGW(TAG, "Nome '%s' nao bateu, usando o unico target configurado ('%s')",
                 name, WOL_TARGETS[0].name);
        return 0;
    }
    return -1;
}

/* Remove espacos/tabs/CR/LF do inicio e do fim de uma string, em memoria */
static void trim(char *s)
{
    char *start = s;
    while (*start == ' ' || *start == '\t') start++;
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                        s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

static bool parse_mac(const char *raw, uint8_t *mac)
{
    char buf[24];
    strncpy(buf, raw, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    trim(buf);

    unsigned int values[6];
    if (sscanf(buf, "%x:%x:%x:%x:%x:%x",
               &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) != 6) {
        ESP_LOGW(TAG, "MAC invalido apos trim: '%s' (original: '%s')", buf, raw);
        return false;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)values[i];
    }
    return true;
}

static esp_err_t send_magic_packet(const uint8_t *mac)
{
    uint8_t packet[102];
    memset(packet, 0xFF, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(packet + 6 + i * 6, mac, 6);
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Falha ao criar socket: errno %d", errno);
        return ESP_FAIL;
    }

    int broadcast_enable = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));

    struct sockaddr_in dest_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(9),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };

    int sent = sendto(sock, packet, sizeof(packet), 0,
                       (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    close(sock);

    if (sent != sizeof(packet)) {
        ESP_LOGE(TAG, "sendto falhou: errno %d", errno);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Pacote magico enviado para %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}

static bool tcp_port_is_open(const char *ip, uint16_t port, uint32_t timeout_ms)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return false;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_pton(AF_INET, ip, &addr.sin_addr);

    bool connected = false;
    int ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == 0) {
        connected = true;
    } else if (errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv = {
            .tv_sec = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };
        if (select(sock + 1, NULL, &wfds, NULL, &tv) > 0) {
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
            connected = (so_error == 0);
        }
    }

    close(sock);
    return connected;
}

static void wol_verify_task(void *arg)
{
    int idx = (int)(intptr_t)arg;
    const wol_target_t *target = &WOL_TARGETS[idx];

    ESP_LOGI(TAG, "[%s] Verificando %s:%u (aguardando %d ms)",
             target->name, target->ip, target->port, WOL_VERIFY_GRACE_MS);
    vTaskDelay(pdMS_TO_TICKS(WOL_VERIFY_GRACE_MS));

    uint32_t deadline = now_ms() + WOL_VERIFY_TIMEOUT_MS;
    bool success = false;

    while (now_ms() < deadline) {
        if (tcp_port_is_open(target->ip, target->port, WOL_VERIFY_CONNECT_TIMEOUT_MS)) {
            success = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(WOL_VERIFY_RETRY_MS));
    }

    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        s_status[idx].state = success ? WOL_VERIFY_SUCCESS : WOL_VERIFY_TIMEOUT;
        s_status[idx].finished_at_ms = now_ms();
        xSemaphoreGive(s_status_mutex);
    }

    ESP_LOGI(TAG, "[%s] Verificacao: %s", target->name,
             success ? "SUCESSO" : "TIMEOUT");

    vTaskDelete(NULL);
}

static void start_verification(int idx)
{
    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        s_status[idx].state = WOL_VERIFY_PENDING;
        s_status[idx].started_at_ms = now_ms();
        s_status[idx].finished_at_ms = 0;
        xSemaphoreGive(s_status_mutex);
    }
    xTaskCreate(wol_verify_task, "wol_verify", 4096, (void *)(intptr_t)idx, 4, NULL);
}

// ============================================================================
// Handlers HTTP
// ============================================================================

static const char INDEX_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"pt-br\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"<title>Wake on LAN</title>\n"
"<style>\n"
"body{background:#111;color:#eee;font-family:sans-serif;max-width:480px;margin:40px auto;padding:0 16px}\n"
"h1{font-size:20px}\n"
".card{background:#1c1c1c;border-radius:8px;padding:16px;margin-bottom:12px}\n"
".name{font-size:16px;font-weight:bold;margin-bottom:8px}\n"
"button{background:#2d7dd2;color:#fff;border:none;padding:10px 16px;border-radius:6px;font-size:14px;cursor:pointer}\n"
"button:disabled{background:#444;cursor:default}\n"
".status{margin-top:8px;font-size:13px;color:#aaa}\n"
".status.success{color:#4caf50}\n"
".status.timeout{color:#e05252}\n"
".status.pending{color:#e0b052}\n"
".docs{margin-top:24px;background:#1c1c1c;border-radius:8px;padding:12px 16px;font-size:13px;color:#ccc}\n"
".docs summary{cursor:pointer;color:#8ab4f8;font-weight:bold}\n"
".docs code{background:#000;padding:2px 5px;border-radius:4px;color:#e0b052;word-break:break-all}\n"
".docs p{margin:10px 0}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<h1>Wake on LAN</h1>\n"
"<div id=\"list\">Carregando...</div>\n"
"<details class=\"docs\">\n"
"<summary>Como usar a API diretamente</summary>\n"
"<p><code>GET /api/targets</code><br>Lista os PCs configurados (JSON).</p>\n"
"<p><code>GET /api/wol?target=NOME</code><br>Envia o pacote magico para o PC \"NOME\" (use o nome exato mostrado acima, com maiusculas/minusculas iguais).</p>\n"
"<p><code>GET /api/status?target=NOME</code><br>Status da verificacao de wake (so funciona se o PC tiver IP preenchido em wol_targets.h).</p>\n"
"<p>Exemplo via curl:<br><code>curl \"http://SEU-IP:8080/api/wol?target=Meu%20PC\"</code></p>\n"
"</details>\n"
"<script>\n"
"const list = document.getElementById(\"list\");\n"
"async function loadTargets() {\n"
"  const res = await fetch(\"/api/targets\");\n"
"  const targets = await res.json();\n"
"  list.innerHTML = \"\";\n"
"  targets.forEach(function(t) {\n"
"    const card = document.createElement(\"div\");\n"
"    card.className = \"card\";\n"
"    const nameDiv = document.createElement(\"div\");\n"
"    nameDiv.className = \"name\";\n"
"    nameDiv.textContent = t.name;\n"
"    const btn = document.createElement(\"button\");\n"
"    btn.textContent = \"Acordar\";\n"
"    const statusDiv = document.createElement(\"div\");\n"
"    statusDiv.className = \"status\";\n"
"    btn.addEventListener(\"click\", function() { wake(t.name, btn, statusDiv); });\n"
"    card.appendChild(nameDiv);\n"
"    card.appendChild(btn);\n"
"    card.appendChild(statusDiv);\n"
"    list.appendChild(card);\n"
"  });\n"
"}\n"
"async function wake(name, btn, statusDiv) {\n"
"  btn.disabled = true;\n"
"  statusDiv.className = \"status pending\";\n"
"  statusDiv.textContent = \"Enviando pacote...\";\n"
"  const res = await fetch(\"/api/wol?target=\" + encodeURIComponent(name));\n"
"  const data = await res.json();\n"
"  if (!res.ok) {\n"
"    statusDiv.className = \"status timeout\";\n"
"    statusDiv.textContent = \"Erro: \" + (data.error || res.status);\n"
"    btn.disabled = false;\n"
"    return;\n"
"  }\n"
"  if (data.verifying) {\n"
"    poll(name, btn, statusDiv);\n"
"  } else {\n"
"    statusDiv.className = \"status\";\n"
"    statusDiv.textContent = \"Pacote enviado\";\n"
"    btn.disabled = false;\n"
"  }\n"
"}\n"
"async function poll(name, btn, statusDiv) {\n"
"  const res = await fetch(\"/api/status?target=\" + encodeURIComponent(name));\n"
"  const data = await res.json();\n"
"  if (data.state === \"pending\") {\n"
"    statusDiv.className = \"status pending\";\n"
"    statusDiv.textContent = \"Aguardando (\" + Math.round(data.elapsed_ms/1000) + \"s)...\";\n"
"    setTimeout(function() { poll(name, btn, statusDiv); }, 3000);\n"
"  } else if (data.state === \"success\") {\n"
"    statusDiv.className = \"status success\";\n"
"    statusDiv.textContent = \"Ligou! (\" + Math.round(data.elapsed_ms/1000) + \"s)\";\n"
"    btn.disabled = false;\n"
"  } else if (data.state === \"timeout\") {\n"
"    statusDiv.className = \"status timeout\";\n"
"    statusDiv.textContent = \"Nao respondeu a tempo\";\n"
"    btn.disabled = false;\n"
"  } else {\n"
"    btn.disabled = false;\n"
"  }\n"
"}\n"
"loadTargets();\n"
"</script>\n"
"</body>\n"
"</html>\n";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, INDEX_HTML);
    return ESP_OK;
}

static esp_err_t api_targets_handler(httpd_req_t *req)
{
    char buf[128 * WOL_TARGET_COUNT + 16];
    size_t off = 0;
    off += snprintf(buf + off, sizeof(buf) - off, "[");
    for (size_t i = 0; i < WOL_TARGET_COUNT; i++) {
        off += snprintf(buf + off, sizeof(buf) - off,
                         "%s{\"name\":\"%s\"}", i > 0 ? "," : "", WOL_TARGETS[i].name);
    }
    off += snprintf(buf + off, sizeof(buf) - off, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t api_wol_handler(httpd_req_t *req)
{
    char query[128] = {0};
    char target_name[48] = {0};
    char token_str[64] = {0};

    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "target", target_name, sizeof(target_name));
        httpd_query_key_value(query, "token", token_str, sizeof(token_str));
    }

    if (strlen(WOL_SHARED_TOKEN) > 0 && strcmp(token_str, WOL_SHARED_TOKEN) != 0) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "{\"error\":\"token invalido\"}\n");
        return ESP_OK;
    }

    int idx = find_target(target_name);
    if (idx < 0) {
        ESP_LOGW(TAG, "Target nao encontrado: '%s'", target_name);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "{\"error\":\"target desconhecido\"}\n");
        return ESP_OK;
    }

    uint8_t mac[6];
    if (!parse_mac(WOL_TARGETS[idx].mac, mac)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"MAC invalido na configuracao\"}\n");
        return ESP_OK;
    }

    if (send_magic_packet(mac) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"falha ao enviar pacote\"}\n");
        return ESP_OK;
    }

    bool has_ip = strlen(WOL_TARGETS[idx].ip) > 0;
    if (has_ip) {
        start_verification(idx);
    }

    httpd_resp_set_type(req, "application/json");
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"verifying\":%s}\n", has_ip ? "true" : "false");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t api_status_handler(httpd_req_t *req)
{
    char query[96] = {0};
    char target_name[48] = {0};

    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "target", target_name, sizeof(target_name));
    }

    int idx = find_target(target_name);
    if (idx < 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "{\"error\":\"target desconhecido\"}\n");
        return ESP_OK;
    }

    wol_status_t snapshot;
    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        snapshot = s_status[idx];
        xSemaphoreGive(s_status_mutex);
    }

    const char *state_str;
    switch (snapshot.state) {
        case WOL_VERIFY_PENDING: state_str = "pending"; break;
        case WOL_VERIFY_SUCCESS: state_str = "success"; break;
        case WOL_VERIFY_TIMEOUT: state_str = "timeout"; break;
        default: state_str = "none"; break;
    }

    char resp[128];
    if (snapshot.state == WOL_VERIFY_NONE) {
        snprintf(resp, sizeof(resp), "{\"state\":\"none\"}\n");
    } else {
        uint32_t elapsed_ms = (snapshot.finished_at_ms > 0
                                ? snapshot.finished_at_ms
                                : now_ms()) - snapshot.started_at_ms;
        snprintf(resp, sizeof(resp), "{\"state\":\"%s\",\"elapsed_ms\":%lu}\n",
                 state_str, (unsigned long)elapsed_ms);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static const httpd_uri_t uri_index        = { .uri = "/",             .method = HTTP_GET, .handler = index_handler };
static const httpd_uri_t uri_api_targets  = { .uri = "/api/targets",  .method = HTTP_GET, .handler = api_targets_handler };
static const httpd_uri_t uri_api_wol      = { .uri = "/api/wol",      .method = HTTP_GET, .handler = api_wol_handler };
static const httpd_uri_t uri_api_status   = { .uri = "/api/status",   .method = HTTP_GET, .handler = api_status_handler };

esp_err_t wol_http_server_start(void)
{
    s_status_mutex = xSemaphoreCreateMutex();
    if (!s_status_mutex) {
        ESP_LOGE(TAG, "Falha ao criar mutex de status");
        return ESP_ERR_NO_MEM;
    }
    memset(s_status, 0, sizeof(s_status));

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.max_uri_handlers = 8;

    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar servidor HTTP: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_api_targets);
    httpd_register_uri_handler(server, &uri_api_wol);
    httpd_register_uri_handler(server, &uri_api_status);

    ESP_LOGI(TAG, "Servidor WoL iniciado na porta %d (%d PCs configurados)",
             config.server_port, (int)WOL_TARGET_COUNT);
    return ESP_OK;
}