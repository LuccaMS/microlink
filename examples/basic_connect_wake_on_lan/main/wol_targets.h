#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct {
    const char *name;   // Nome de exibição na página
    const char *mac;    // Endereço MAC: "AA:BB:CC:DD:EE:FF"
    const char *ip;     // Deixe "" se não souber / o IP mudar com DHCP.
                         // Com "" o ESP só envia o pacote mágico, sem
                         // tentar confirmar se o PC realmente acordou.
    uint16_t port;       // Só importa se "ip" estiver preenchido.
} wol_target_t;

static const wol_target_t WOL_TARGETS[] = {
    { "Meu PC", "9C:6B:00:A3:D3:A3", "", 0 },
};

#define WOL_TARGET_COUNT (sizeof(WOL_TARGETS) / sizeof(WOL_TARGETS[0]))