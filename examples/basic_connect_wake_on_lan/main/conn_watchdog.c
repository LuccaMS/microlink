#include "conn_watchdog.h"

#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "conn_watchdog";

#define CHECK_INTERVAL_MS 10000

typedef struct {
    microlink_t *ml;
    uint32_t max_disconnected_ms;
} watchdog_args_t;

static void watchdog_task(void *arg)
{
    watchdog_args_t *args = (watchdog_args_t *)arg;
    TickType_t disconnected_since = 0;
    bool was_connected = true; /* assume conectado ao iniciar a task */

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));

        bool connected = microlink_is_connected(args->ml);
        TickType_t now = xTaskGetTickCount();

        if (connected) {
            if (!was_connected) {
                ESP_LOGI(TAG, "MicroLink reconectou, contador zerado");
            }
            was_connected = true;
            disconnected_since = 0;
            continue;
        }

        /* desconectado agora */
        if (was_connected || disconnected_since == 0) {
            disconnected_since = now;
            ESP_LOGW(TAG, "MicroLink desconectado, iniciando contagem do watchdog");
        }
        was_connected = false;

        uint32_t disconnected_ms = (uint32_t)((now - disconnected_since) * portTICK_PERIOD_MS);
        if (disconnected_ms >= args->max_disconnected_ms) {
            ESP_LOGE(TAG, "Desconectado ha %lu ms (limite %lu ms). Reiniciando o ESP32...",
                     (unsigned long)disconnected_ms,
                     (unsigned long)args->max_disconnected_ms);
            vTaskDelay(pdMS_TO_TICKS(500)); /* da tempo do log sair pela serial */
            esp_restart();
        }
    }
}

esp_err_t conn_watchdog_start(microlink_t *ml, uint32_t max_disconnected_ms)
{
    watchdog_args_t *args = malloc(sizeof(watchdog_args_t));
    if (!args) {
        return ESP_ERR_NO_MEM;
    }
    args->ml = ml;
    args->max_disconnected_ms = max_disconnected_ms;

    BaseType_t ok = xTaskCreate(watchdog_task, "conn_watchdog", 4096, args, 5, NULL);
    if (ok != pdPASS) {
        free(args);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Watchdog de conexao iniciado (limite: %lu ms desconectado)",
             (unsigned long)max_disconnected_ms);
    return ESP_OK;
}