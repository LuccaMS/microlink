#pragma once

#include "esp_err.h"
#include "microlink.h"

/**
 * Inicia uma task que monitora o estado de conexão do MicroLink.
 * Se ficar desconectado por mais de max_disconnected_ms consecutivos,
 * reinicia o ESP32 (esp_restart) — útil para destravar situações em
 * que o túnel WireGuard trava sem se recuperar sozinho.
 *
 * Chame depois de microlink_start(), a qualquer momento (não precisa
 * esperar a primeira conexão — a contagem só começa a valer quando
 * ele detectar uma desconexão de fato).
 */
esp_err_t conn_watchdog_start(microlink_t *ml, uint32_t max_disconnected_ms);