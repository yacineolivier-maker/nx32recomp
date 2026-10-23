/* Outils pour les remplacements natifs (HLE) : mémoire invitée gérée par l'hôte, appels de fonctions invitées. */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "cpu.h"
#include "table.h"

void *rt_commit(uint32_t addr, uint32_t size);

/* Zone [0xE0000000, 0xE8000000) : hors image, tas, piles et zone d'alias ; allouée par l'hôte, jamais libérée */
#define NATIF_BASE 0xE0000000u
#define NATIF_FIN 0xE8000000u
static CRITICAL_SECTION g_natif_verrou;
static uint32_t g_natif_pos = NATIF_BASE, g_natif_engage = NATIF_BASE;
static int g_natif_init;

uint32_t natif_alloc(uint32_t taille, uint32_t align) {
    if (!g_natif_init) { InitializeCriticalSection(&g_natif_verrou); g_natif_init = 1; }
    EnterCriticalSection(&g_natif_verrou);
    if (align < 16) align = 16;
    uint32_t a = (g_natif_pos + align - 1) & ~(align - 1);
    if (a + taille > NATIF_FIN) rt_fatal(NULL, "zone native épuisée (%u octets demandés)", taille);
    g_natif_pos = a + taille;
    if (g_natif_pos > g_natif_engage) {
        uint32_t fin = (g_natif_pos + 0xFFFFu) & ~0xFFFFu;
        rt_commit(g_natif_engage, fin - g_natif_engage);
        g_natif_engage = fin;
    }
    LeaveCriticalSection(&g_natif_verrou);
    memset(g_mem + a, 0, taille);
    return a;
}

/* Appel d'une fonction invitée depuis du code natif, sur le thread invité courant (registres préservés) */
uint32_t natif_appel(Cpu *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    Cpu sauve = *c;
    c->r[0] = a0; c->r[1] = a1; c->r[2] = a2; c->r[3] = a3;
    c->r[14] = 0;
    rt_call_indirect(c, fn);
    uint32_t r = c->r[0];
    *c = sauve;
    return r;
}

/* Variante à 6 arguments (les deux derniers sur la pile invitée) */
uint32_t natif_appel6(Cpu *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
    Cpu sauve = *c;
    c->r[13] -= 16;
    wr32(c->r[13], a4);
    wr32(c->r[13] + 4, a5);
    c->r[0] = a0; c->r[1] = a1; c->r[2] = a2; c->r[3] = a3;
    c->r[14] = 0;
    rt_call_indirect(c, fn);
    uint32_t r = c->r[0];
    *c = sauve;
    return r;
}
