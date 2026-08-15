#include "wol.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "wol";

// ---------------------------------------------------------------------
// CONFIGURE AQUI:
#define WOL_DEFAULT_MAC "9C:6B:00:A3:D3:A3"
#define WOL_DEFAULT_IP  ""     // ex: "192.168.0.50" — vazio desativa verificação por padrão
#define WOL_DEFAULT_PORT 3389  // RDP. Use 22 para SSH, etc.

// Token opcional pra proteger o endpoint. "" desativa a checagem.
#define WOL_SHARED_TOKEN ""

// Janela de verificação de wake
#define WOL_VERIFY_GRACE_MS     5000    // espera antes da 1a tentativa
#define WOL_VERIFY_RETRY_MS     3000    // intervalo entre tentativas
#define WOL_VERIFY_TIMEOUT_MS   120000  // desiste depois disso
#define WOL_VERIFY_CONNECT_TIMEOUT_MS 2000
// ---------------------------------------------------------------------

typedef enum {
    WOL_VERIFY_NONE = 0,
    WOL_VERIFY_PENDING,
    WOL_VERIFY_SUCCESS,
    WOL_VERIFY_TIMEOUT,
} wol_verify_state_t;

typedef struct {
    wol_verify_state_t state;
    char target_ip[16];
    uint16_t target_port;
    uint32_t started_at_ms;
    uint32_t finished_at_ms; // 0 se ainda pending
} wol_status_t;

static wol_status_t s_status = { .state = WOL_VERIFY_NONE };
static SemaphoreHandle_t s_status_mutex = NULL;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool parse_mac(const char *str, uint8_t *mac)
{
    unsigned int values[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x",
               &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) != 6) {
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

/* Tenta uma conexão TCP curta pra ver se a porta já está respondendo */
static bool tcp_port_is_open(const char *ip, uint16_t port, uint32_t timeout_ms)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return false;
    }

    /* socket não-bloqueante pra poder aplicar timeout no connect */
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
    char ip[16];
    uint16_t port;

    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        strncpy(ip, s_status.target_ip, sizeof(ip));
        port = s_status.target_port;
        xSemaphoreGive(s_status_mutex);
    } else {
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Verificando wake de %s:%u (aguardando %d ms antes da 1a tentativa)",
             ip, port, WOL_VERIFY_GRACE_MS);
    vTaskDelay(pdMS_TO_TICKS(WOL_VERIFY_GRACE_MS));

    uint32_t deadline = now_ms() + WOL_VERIFY_TIMEOUT_MS;
    bool success = false;

    while (now_ms() < deadline) {
        if (tcp_port_is_open(ip, port, WOL_VERIFY_CONNECT_TIMEOUT_MS)) {
            success = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(WOL_VERIFY_RETRY_MS));
    }

    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        s_status.state = success ? WOL_VERIFY_SUCCESS : WOL_VERIFY_TIMEOUT;
        s_status.finished_at_ms = now_ms();
        xSemaphoreGive(s_status_mutex);
    }

    ESP_LOGI(TAG, "Verificacao de %s:%u: %s", ip, port,
             success ? "SUCESSO (porta respondeu)" : "TIMEOUT (nao respondeu a tempo)");

    vTaskDelete(NULL);
}

static void start_verification(const char *ip, uint16_t port)
{
    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        s_status.state = WOL_VERIFY_PENDING;
        strncpy(s_status.target_ip, ip, sizeof(s_status.target_ip) - 1);
        s_status.target_ip[sizeof(s_status.target_ip) - 1] = '\0';
        s_status.target_port = port;
        s_status.started_at_ms = now_ms();
        s_status.finished_at_ms = 0;
        xSemaphoreGive(s_status_mutex);
    }

    xTaskCreate(wol_verify_task, "wol_verify", 4096, NULL, 4, NULL);
}

static esp_err_t wol_get_handler(httpd_req_t *req)
{
    char query[128] = {0};
    char mac_str[32] = {0};
    char ip_str[32] = {0};
    char port_str[8] = {0};
    char token_str[64] = {0};

    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "mac", mac_str, sizeof(mac_str));
        httpd_query_key_value(query, "ip", ip_str, sizeof(ip_str));
        httpd_query_key_value(query, "port", port_str, sizeof(port_str));
        httpd_query_key_value(query, "token", token_str, sizeof(token_str));
    }

    if (strlen(WOL_SHARED_TOKEN) > 0 && strcmp(token_str, WOL_SHARED_TOKEN) != 0) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "Token invalido ou ausente\n");
        return ESP_OK;
    }

    if (strlen(mac_str) == 0) {
        strncpy(mac_str, WOL_DEFAULT_MAC, sizeof(mac_str) - 1);
    }
    if (strlen(ip_str) == 0) {
        strncpy(ip_str, WOL_DEFAULT_IP, sizeof(ip_str) - 1);
    }
    uint16_t port = strlen(port_str) > 0 ? (uint16_t)atoi(port_str) : WOL_DEFAULT_PORT;

    uint8_t mac[6];
    if (!parse_mac(mac_str, mac)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "MAC invalido. Use ?mac=AA:BB:CC:DD:EE:FF\n");
        return ESP_OK;
    }

    if (send_magic_packet(mac) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Falha ao enviar pacote magico\n");
        return ESP_OK;
    }

    if (strlen(ip_str) > 0) {
        start_verification(ip_str, port);
        char resp[160];
        snprintf(resp, sizeof(resp),
                 "Pacote magico enviado. Verificando %s:%u em background "
                 "(consulte /wol/status)\n", ip_str, port);
        httpd_resp_sendstr(req, resp);
    } else {
        httpd_resp_sendstr(req, "Pacote magico enviado (sem verificacao, IP nao informado)\n");
    }

    return ESP_OK;
}

static esp_err_t wol_status_handler(httpd_req_t *req)
{
    char resp[256];
    wol_status_t snapshot;

    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        snapshot = s_status;
        xSemaphoreGive(s_status_mutex);
    } else {
        httpd_resp_sendstr(req, "erro interno\n");
        return ESP_OK;
    }

    const char *state_str;
    switch (snapshot.state) {
        case WOL_VERIFY_PENDING: state_str = "pending"; break;
        case WOL_VERIFY_SUCCESS: state_str = "success"; break;
        case WOL_VERIFY_TIMEOUT: state_str = "timeout"; break;
        default: state_str = "none"; break;
    }

    if (snapshot.state == WOL_VERIFY_NONE) {
        snprintf(resp, sizeof(resp), "{\"state\":\"none\"}\n");
    } else {
        uint32_t elapsed_ms = (snapshot.finished_at_ms > 0
                                ? snapshot.finished_at_ms
                                : now_ms()) - snapshot.started_at_ms;
        snprintf(resp, sizeof(resp),
                 "{\"state\":\"%s\",\"target_ip\":\"%s\",\"target_port\":%u,"
                 "\"elapsed_ms\":%lu}\n",
                 state_str, snapshot.target_ip, snapshot.target_port,
                 (unsigned long)elapsed_ms);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static const httpd_uri_t wol_uri = {
    .uri = "/wol",
    .method = HTTP_GET,
    .handler = wol_get_handler,
};

static const httpd_uri_t wol_status_uri = {
    .uri = "/wol/status",
    .method = HTTP_GET,
    .handler = wol_status_handler,
};

esp_err_t wol_http_server_start(void)
{
    s_status_mutex = xSemaphoreCreateMutex();
    if (!s_status_mutex) {
        ESP_LOGE(TAG, "Falha ao criar mutex de status");
        return ESP_ERR_NO_MEM;
    }

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.max_uri_handlers = 8;

    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar servidor HTTP: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_register_uri_handler(server, &wol_uri);
    httpd_register_uri_handler(server, &wol_status_uri);
    ESP_LOGI(TAG, "Servidor WoL iniciado na porta %d (/wol, /wol/status)", config.server_port);
    return ESP_OK;
}