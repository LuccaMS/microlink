#pragma once

#include "esp_err.h"

/**
 * Inicia o servidor HTTP com os endpoints:
 *
 *   GET /wol?mac=AA:BB:CC:DD:EE:FF&ip=192.168.0.50&port=3389
 *     Envia o pacote mágico e, se "ip" for informado, dispara uma
 *     verificação em background tentando abrir uma conexão TCP na
 *     porta indicada (padrão 3389/RDP) até o PC responder ou dar timeout.
 *     Responde imediatamente, não espera a verificação terminar.
 *
 *   GET /wol/status
 *     Retorna o resultado da última verificação (pending/success/timeout).
 *
 * Parâmetros omitidos usam os valores padrão definidos no topo do wol.c
 * (WOL_DEFAULT_MAC, WOL_DEFAULT_IP, WOL_DEFAULT_PORT).
 *
 * Chame depois que o WiFi já estiver conectado (não depende do MicroLink).
 */
esp_err_t wol_http_server_start(void);