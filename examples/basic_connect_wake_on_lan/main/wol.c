#include "wol.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "lwip/sockets.h"

static const char *TAG = "wol";

// ---------------------------------------------------------------------
// CONFIGURE AQUI:
// MAC do PC que você quer acordar (usado se ?mac= não for passado na URL).
// Formato: AA:BB:CC:DD:EE:FF
#define WOL_DEFAULT_MAC "9C:6B:00:A3:D3:A3"

// Token simples opcional pra evitar que qualquer coisa na sua tailnet
// acorde o PC sem querer. Deixe "" para desativar a checagem.
// Se preenchido, a URL vira: /wol?mac=...&token=SEU_TOKEN
#define WOL_SHARED_TOKEN ""
// ---------------------------------------------------------------------

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
    // Pacote mágico: 6 bytes 0xFF + MAC repetido 16 vezes = 102 bytes
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
        .sin_port = htons(9), // porta padrão WoL (algumas placas usam 7)
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

static esp_err_t wol_get_handler(httpd_req_t *req)
{
    char query[96] = {0};
    char mac_str[32] = {0};
    char token_str[64] = {0};

    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "mac", mac_str, sizeof(mac_str));
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

    uint8_t mac[6];
    if (!parse_mac(mac_str, mac)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "MAC invalido. Use ?mac=AA:BB:CC:DD:EE:FF\n");
        return ESP_OK;
    }

    if (send_magic_packet(mac) == ESP_OK) {
        httpd_resp_sendstr(req, "Pacote magico enviado\n");
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Falha ao enviar pacote magico\n");
    }
    return ESP_OK;
}

static const httpd_uri_t wol_uri = {
    .uri = "/wol",
    .method = HTTP_GET,
    .handler = wol_get_handler,
};

esp_err_t wol_http_server_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Porta 8080 pra nao conflitar com o config server do MicroLink (porta 80),
    // caso voce tenha ativado CONFIG_ML_ENABLE_CONFIG_HTTPD.
    config.server_port = 8080;

    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar servidor HTTP: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_register_uri_handler(server, &wol_uri);
    ESP_LOGI(TAG, "Servidor WoL iniciado na porta %d (rota /wol)", config.server_port);
    return ESP_OK;
}
