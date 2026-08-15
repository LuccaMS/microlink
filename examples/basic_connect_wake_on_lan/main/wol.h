#pragma once

#include "esp_err.h"

/**
 * Inicia um servidor HTTP simples com o endpoint GET /wol.
 *
 * Uso: http://<ip-do-esp>:8080/wol?mac=AA:BB:CC:DD:EE:FF
 * Se o parâmetro "mac" não for enviado, usa WOL_DEFAULT_MAC (definido em wol.c).
 *
 * Chame esta função depois que o WiFi já estiver conectado
 * (não precisa esperar o MicroLink/Tailscale — o pacote WoL é
 * enviado por broadcast na rede local, não pelo túnel).
 */
esp_err_t wol_http_server_start(void);
