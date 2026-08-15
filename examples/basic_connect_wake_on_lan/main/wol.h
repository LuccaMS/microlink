#pragma once

#include "esp_err.h"

/**
 * Inicia o servidor HTTP (porta 8080) com:
 *
 *   GET  /                     Página HTML com um botão por PC.
 *   GET  /api/targets          JSON com a lista de PCs configurados.
 *   GET  /api/wol?target=NOME  Envia o pacote mágico. Se o alvo tiver
 *                              "ip" preenchido em wol_targets.h, também
 *                              dispara verificação de wake em background.
 *   GET  /api/status?target=NOME
 *                              Estado da verificação (se configurada).
 *
 * Edite main/wol_targets.h para adicionar/remover PCs ou preencher o IP.
 *
 * Chame depois que o WiFi já estiver conectado.
 */
esp_err_t wol_http_server_start(void);