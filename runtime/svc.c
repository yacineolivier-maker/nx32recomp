/* Appels système Horizon (ABI AArch32) : mémoire, threads, synchronisation, journalisation IPC.
 * Conventions de registres reprises des enveloppes « 64From32 » du noyau (cf. hle/kernel/svc de yuzu/Eden). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>

#include "cpu.h"
#include "svc.h"
#include "table.h"
#include "jeu.h"

uint32_t ipc_dispatch(Cpu *c, uint32_t handle);
void rt_trace_register(uint32_t handle);
uint32_t ipc_connect(const char *name);
void ipc_init(void);

#define RES_OK 0u
#define RES_INVALID_HANDLE 0xE401u
#define RES_TIMEOUT 0xEA01u
#define RES_CANCELLED 0xEC01u
#define RES_INVALID_STATE 0xFA01u
#define RES_INVALID_ADDRESS 0xCC01u
#define RES_NOT_FOUND 0xF201u
#define RES_OUT_OF_RANGE 0xF401u
#define WAITER_BIT 0x40000000u
#define PSEUDO_THREAD 0xFFFF8000u
#define PSEUDO_PROCESS 0xFFFF8001u

/* ------------------------------------------------------------------ objets noyau */
typedef enum { OBJ_FREE, OBJ_THREAD, OBJ_EVENT_R, OBJ_EVENT_W, OBJ_SESSION, OBJ_SHMEM, OBJ_OTHER } ObjType;

/* Mémoire partagée : le serveur (runtime hôte) écrit à l'adresse invitée où le jeu l'a mappée. */
typedef struct ShMem { uint32_t size, addr; const uint8_t *init; } ShMem; /* init : contenu copié au mappage */

typedef struct Thread Thread;
typedef struct Event { int signaled; } Event;

typedef struct Obj {
    ObjType type;
    union { Thread *thread; Event *event; void *p; };
    char name[16];
} Obj;

struct Thread {
    Cpu cpu;
    uint32_t handle;
    uint32_t entry, arg;
    int32_t priority, core;
    int32_t coeur_tenu;   /* cœur invité dont ce thread détient le jeton (-1 : aucun) */
    volatile int *preempt; /* drapeau rc_preempt du thread hôte */
    int a_reprendre;       /* le cœur a été rendu pendant l'appel système : à reprendre en sortie */
    HANDLE host;
    CONDITION_VARIABLE cv;
    int wake;           /* réveil demandé */
    uint32_t wait_addr; /* adresse de mutex ou de clé attendue */
    uint32_t wait_key;
    int waiting_kind;   /* 1 mutex, 2 clé cv, 3 adresse */
    uint32_t result;
    int exited;
    int annule;         /* CancelSynchronization en attente */
    volatile uint32_t svc_en_cours, svc_lr, svc_args[4]; /* diagnostic (NX_DUMP_THREADS) : appel système en cours */
    Thread *next_wait;
};

#define MAX_OBJ 4096
static Obj g_obj[MAX_OBJ];
static CRITICAL_SECTION g_lock; /* « verrou du noyau » : protège objets et files d'attente */
static DWORD g_tls_slot;
static uint32_t g_heap_size;
static uint32_t g_next_tls = TLS_BASE;

static Thread *g_waiters; /* liste simple de tous les threads en attente (mutex, cv, adresse) */

static uint32_t new_handle(ObjType t, void *p, const char *name) {
    for (int i = 1; i < MAX_OBJ; i++)
        if (g_obj[i].type == OBJ_FREE) {
            g_obj[i].type = t;
            g_obj[i].p = p;
            snprintf(g_obj[i].name, sizeof g_obj[i].name, "%s", name ? name : "");
            return 0x00010000u | (uint32_t)i;
        }
    rt_fatal(NULL, "table des handles pleine");
    return 0;
}

static Thread *cur_thread(void) { return (Thread *)TlsGetValue(g_tls_slot); }
Cpu *svc_current(void) { Thread *t = cur_thread(); return t ? &t->cpu : NULL; }

static Obj *get_obj(uint32_t h) {
    if (h == PSEUDO_THREAD) {
        static Obj tmp;
        tmp.type = OBJ_THREAD;
        tmp.thread = cur_thread();
        return &tmp;
    }
    uint32_t i = h & 0xFFFF;
    if ((h & 0xFFFF0000u) != 0x00010000u || i == 0 || i >= MAX_OBJ || g_obj[i].type == OBJ_FREE) return NULL;
    return &g_obj[i];
}

/* ------------------------------------------------------------------ temps */
static LARGE_INTEGER g_qpf, g_qpc0;
static uint64_t ticks(void) { /* horloge système Switch : 19,2 MHz */
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return (uint64_t)((double)(q.QuadPart - g_qpc0.QuadPart) * 19200000.0 / (double)g_qpf.QuadPart);
}

uint64_t rt_ticks(void) { return ticks(); }

/* ------------------------------------------------------------------ attente générique */
static void wait_list_add(Thread *t) { t->next_wait = g_waiters; g_waiters = t; }
static void wait_list_remove(Thread *t) {
    for (Thread **p = &g_waiters; *p; p = &(*p)->next_wait)
        if (*p == t) { *p = t->next_wait; return; }
}
/* Attend un réveil (verrou noyau tenu). timeout_ns < 0 : infini. Renvoie 0 si réveillé, 1 si délai écoulé. */
static void coeur_rendre(Thread *t);
static int coeur_convoite(Thread *t);
static int block(Thread *t, int64_t timeout_ns) {
    if (t->coeur_tenu >= 0) { coeur_rendre(t); t->a_reprendre = 1; } /* blocage réel : le cœur est libéré */
    t->wake = 0;
    while (!t->wake) {
        DWORD ms = timeout_ns < 0 ? INFINITE : (DWORD)((timeout_ns + 999999) / 1000000);
        if (!SleepConditionVariableCS(&t->cv, &g_lock, ms) && GetLastError() == ERROR_TIMEOUT && !t->wake)
            return 1;
        if (timeout_ns >= 0 && !t->wake) return 1;
    }
    return 0;
}
static void wake(Thread *t) { t->wake = 1; WakeConditionVariable(&t->cv); }
/* Réveille tous les threads bloqués dans WaitSynchronization pour qu'ils revérifient leurs handles. */
static void wake_all_sync(void) {
    for (Thread *w = g_waiters; w; w = w->next_wait)
        if (w->waiting_kind == 4) wake(w);
}

/* ------------------------------------------------------------------ écritures noyau sur des mots partagés
 * Le jeu manipule les mêmes mots (mutex, clés, adresses) avec ldrex/strex : le noyau passe par le même verrou
 * d'emplacement et incrémente la version, ce qui fait échouer tout strex concurrent (atomicité comme sur la console). */
static uint32_t k_verrou(uint32_t a) {
    uint32_t h = (a >> 3) & 4095;
    while (__sync_lock_test_and_set(&rt_excl_verrou[h], 1)) __builtin_ia32_pause();
    return h;
}
static void k_deverrou(uint32_t h) { rt_excl_seq[h]++; __sync_lock_release(&rt_excl_verrou[h]); }
static void k_store(uint32_t a, uint32_t v) {
    uint32_t h = k_verrou(a);
    __atomic_store_n((uint32_t *)HP(a), v, __ATOMIC_RELEASE);
    k_deverrou(h);
}

/* ------------------------------------------------------------------ mutex invités (ArbitrateLock/Unlock) */
static Thread *thread_by_handle(uint32_t h) { Obj *o = get_obj(h); return o && o->type == OBJ_THREAD ? o->thread : NULL; }

/* Transfère un mutex libéré au plus prioritaire des threads qui l'attendent. */
static void mutex_release(uint32_t addr) {
    Thread *best = NULL;
    int nb = 0;
    for (Thread *w = g_waiters; w; w = w->next_wait)
        if (w->waiting_kind == 1 && w->wait_addr == addr) {
            nb++;
            if (!best || w->priority < best->priority) best = w;
        }
    if (!best) { k_store(addr, 0); return; }
    k_store(addr, best->handle | (nb > 1 ? WAITER_BIT : 0));
    wait_list_remove(best);
    best->waiting_kind = 0;
    best->result = RES_OK;
    wake(best);
}

static uint32_t arbitrate_lock(Thread *t, uint32_t owner, uint32_t addr, uint32_t tag) {
    EnterCriticalSection(&g_lock);
    if (rd32(addr) != (owner | WAITER_BIT)) { LeaveCriticalSection(&g_lock); return RES_OK; }
    t->waiting_kind = 1;
    t->wait_addr = addr;
    wait_list_add(t);
    block(t, -1);
    LeaveCriticalSection(&g_lock);
    return t->result;
}

/* ------------------------------------------------------------------ IPC (journalisation pour l'instant) */
static void log_ipc(Cpu *c, uint32_t h) {
    uint32_t tls = c->tpidruro;
    uint32_t w0 = rd32(tls), w1 = rd32(tls + 4);
    Obj *o = get_obj(h);
    fprintf(stderr, "[ipc] SendSyncRequest h=%08X (%s) type=%u words=%u\n", h, o ? o->name : "?", w0 & 0xFFFF, w1 & 0x3FF);
    for (int i = 0; i < 16; i++) fprintf(stderr, "%08X%s", rd32(tls + 4 * i), i % 8 == 7 ? "\n" : " ");
}

/* ------------------------------------------------------------------ sessions et événements (pour ipc.cpp) */
uint32_t svc_new_session(void *obj) {
    EnterCriticalSection(&g_lock);
    uint32_t h = new_handle(OBJ_SESSION, obj, "session");
    LeaveCriticalSection(&g_lock);
    return h;
}
void *svc_session_obj(uint32_t h) {
    Obj *o = get_obj(h);
    return o && o->type == OBJ_SESSION ? o->p : NULL;
}
/* Événement : un objet partagé, deux handles (lecture pour le client, écriture pour le serveur). */
uint32_t svc_new_shmem(uint32_t size) {
    ShMem *m = calloc(1, sizeof *m);
    m->size = size;
    EnterCriticalSection(&g_lock);
    uint32_t h = new_handle(OBJ_SHMEM, m, "shmem");
    LeaveCriticalSection(&g_lock);
    return h;
}
void svc_shmem_set_init(uint32_t h, const uint8_t *init) {
    Obj *o = get_obj(h);
    if (o && o->type == OBJ_SHMEM) ((ShMem *)o->p)->init = init;
}
/* Adresse invitée de la mémoire partagée (0 tant que le jeu ne l'a pas mappée) */
uint32_t svc_shmem_addr(uint32_t h) {
    Obj *o = get_obj(h);
    return o && o->type == OBJ_SHMEM ? ((ShMem *)o->p)->addr : 0;
}

/* Copie d'un handle par IPC : nouveau handle vers le même objet (fermer l'un ne ferme pas l'autre) */
uint32_t svc_dupliquer_handle(uint32_t h) {
    EnterCriticalSection(&g_lock);
    Obj *o = get_obj(h);
    uint32_t d = o ? new_handle(o->type, o->p, o->name) : h;
    LeaveCriticalSection(&g_lock);
    return d;
}

const char *ipc_contexte(void); /* ipc.cpp : service#commande en cours sur ce thread */
uint32_t svc_new_event(int signaled, uint32_t *readable) {
    Event *e = calloc(1, sizeof *e);
    e->signaled = signaled;
    const char *ctx = ipc_contexte();
    const char *nom = ctx[0] ? ctx : "event";
    /* nom court : fin du nom de service (après le dernier ':') et numéro de commande */
    const char *deux_points = strrchr(nom, ':');
    if (deux_points && deux_points[1]) nom = deux_points + 1;
    EnterCriticalSection(&g_lock);
    uint32_t w = new_handle(OBJ_EVENT_W, e, nom);
    uint32_t rd = new_handle(OBJ_EVENT_R, e, nom);
    LeaveCriticalSection(&g_lock);
    if (readable) *readable = rd;
    return w;
}
static void wake_all_sync(void);
/* événement remis à zéro par un service (file vidée, etc.) : sinon les attentes du jeu reviennent immédiatement */
void svc_clear_event(uint32_t h) {
    EnterCriticalSection(&g_lock);
    Obj *o = get_obj(h);
    if (o && (o->type == OBJ_EVENT_W || o->type == OBJ_EVENT_R)) o->event->signaled = 0;
    LeaveCriticalSection(&g_lock);
}
void svc_signal_event(uint32_t h) {
    EnterCriticalSection(&g_lock);
    Obj *o = get_obj(h);
    if (o && (o->type == OBJ_EVENT_W || o->type == OBJ_EVENT_R)) o->event->signaled = 1;
    wake_all_sync();
    LeaveCriticalSection(&g_lock);
}

/* ------------------------------------------------------------------ ordonnancement par cœur (modèle Horizon)
 * Sur la console, deux threads d'un même cœur ne s'exécutent jamais simultanément et un thread de priorité plus
 * basse ne prend pas la main tant qu'un plus prioritaire est exécutable. Le jeu en dépend (files de travaux).
 * Modèle : un jeton par cœur ; on le rend à chaque appel système bloquant, le plus prioritaire le reprend. */
#define NB_COEURS 3
static CRITICAL_SECTION g_sched;
static CONDITION_VARIABLE g_sched_cv;
static Thread *g_coeur[NB_COEURS];
static Thread *g_attente_coeur[256];
static int g_nb_attente;

static int g_un_coeur = -1; /* diagnostic NX_UN_COEUR : tous les threads sur le cœur 0 */
static int coeur_de(Thread *t) {
    if (g_un_coeur < 0) g_un_coeur = getenv("NX_UN_COEUR") != NULL;
    if (g_un_coeur) return 0;
    return t->core >= 0 && t->core < NB_COEURS ? t->core : 0;
}

__declspec(thread) volatile int rc_preempt;

static void coeur_prendre(Thread *t) {
    if (!t) return;
    t->preempt = &rc_preempt;
    int k = coeur_de(t);
    EnterCriticalSection(&g_sched);
    if (g_coeur[k] == t) { t->coeur_tenu = k; LeaveCriticalSection(&g_sched); return; } /* déjà détenteur (reprise imbriquée) */
    g_attente_coeur[g_nb_attente++] = t;
    DWORD debut = GetTickCount();
    for (;;) {
        int prioritaire = 1; /* aucun autre en attente sur ce cœur n'est plus prioritaire */
        for (int i = 0; i < g_nb_attente; i++) {
            Thread *w = g_attente_coeur[i];
            if (w != t && coeur_de(w) == k && w->priority < t->priority) { prioritaire = 0; break; }
        }
        if (!g_coeur[k] && prioritaire) break;
        if (g_coeur[k] && g_coeur[k]->priority > t->priority && g_coeur[k]->preempt)
            *g_coeur[k]->preempt = 1; /* demander au détenteur moins prioritaire de céder */
        if (GetTickCount() - debut > 500) { /* soupape : éviter un interblocage (attente active sur un même cœur) */
            fprintf(stderr, "[sched] soupape : thread %08X passe sur le cœur %d occupé par %08X\n", t->handle, k,
                    g_coeur[k] ? g_coeur[k]->handle : 0);
            break;
        }
        SleepConditionVariableCS(&g_sched_cv, &g_sched, 20);
    }
    for (int i = 0; i < g_nb_attente; i++)
        if (g_attente_coeur[i] == t) { g_attente_coeur[i] = g_attente_coeur[--g_nb_attente]; break; }
    if (!g_coeur[k]) g_coeur[k] = t;
    t->coeur_tenu = g_coeur[k] == t ? k : -1;
    LeaveCriticalSection(&g_sched);
}

/* Un autre thread attend-il le cœur de t ? */
static int coeur_convoite(Thread *t) {
    int r = 0;
    EnterCriticalSection(&g_sched);
    for (int i = 0; i < g_nb_attente; i++)
        if (g_attente_coeur[i] != t && coeur_de(g_attente_coeur[i]) == coeur_de(t)) { r = 1; break; }
    LeaveCriticalSection(&g_sched);
    return r;
}

static void coeur_rendre(Thread *t) {
    if (!t) return;
    EnterCriticalSection(&g_sched);
    if (t->coeur_tenu >= 0 && g_coeur[t->coeur_tenu] == t) g_coeur[t->coeur_tenu] = NULL;
    t->coeur_tenu = -1;
    WakeAllConditionVariable(&g_sched_cv);
    LeaveCriticalSection(&g_sched);
}

volatile long g_nb_preempt;
void rt_preempt(void) { /* appelé à l'entrée d'une fonction quand un thread plus prioritaire attend le cœur */
    Thread *t = cur_thread();
    InterlockedIncrement(&g_nb_preempt);
    rc_preempt = 0;
    coeur_rendre(t);
    coeur_prendre(t);
}
/* Attente « matérielle » (GPU, synchronisation verticale) : le cœur est rendu pendant l'attente */
void svc_attente_debut(void) { coeur_rendre(cur_thread()); }
void svc_attente_fin(void) { coeur_prendre(cur_thread()); }

/* ------------------------------------------------------------------ threads */
static DWORD WINAPI thread_main(LPVOID p) {
    Thread *t = (Thread *)p;
    TlsSetValue(g_tls_slot, t);
    rt_trace_register(t->handle);
    coeur_prendre(t);
    t->cpu.r[14] = 0;
    rt_call_indirect(&t->cpu, t->entry);
    coeur_rendre(t);
    EnterCriticalSection(&g_lock);
    t->exited = 1;
    wake_all_sync();
    LeaveCriticalSection(&g_lock);
    return 0;
}

uint32_t svc_alloc_tls(Cpu *c) {
    uint32_t a = g_next_tls;
    g_next_tls += 0x200;
    memset(g_mem + a, 0, 0x200);
    return a;
}

/* piles hôtes de tous les threads invités (diagnostic de blocage) : module+décalage, à symboliser hors ligne */
void svc_piles_hotes(void) {
    static int init;
    if (!init) { SymInitialize(GetCurrentProcess(), NULL, TRUE); init = 1; }
    for (int i = 0; i < MAX_OBJ; i++) {
        if (g_obj[i].type != OBJ_THREAD || !g_obj[i].thread) continue;
        Thread *t = g_obj[i].thread;
        if (t->exited || !t->host || GetThreadId(t->host) == GetCurrentThreadId()) continue;
        if (SuspendThread(t->host) == (DWORD)-1) continue;
        CONTEXT ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_FULL;
        fprintf(stderr, "[pile-hote] %08X :", t->handle);
        if (GetThreadContext(t->host, &ctx)) {
            STACKFRAME64 f;
            memset(&f, 0, sizeof f);
            f.AddrPC.Offset = ctx.Rip; f.AddrPC.Mode = AddrModeFlat;
            f.AddrFrame.Offset = ctx.Rbp; f.AddrFrame.Mode = AddrModeFlat;
            f.AddrStack.Offset = ctx.Rsp; f.AddrStack.Mode = AddrModeFlat;
            for (int k = 0; k < 14; k++) {
                HMODULE m = NULL;
                char nom[MAX_PATH] = "?";
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)f.AddrPC.Offset, &m))
                    GetModuleFileNameA(m, nom, sizeof nom);
                const char *court = strrchr(nom, 92) ? strrchr(nom, 92) + 1 : nom;
                fprintf(stderr, " %s+%llX", court, (unsigned long long)(f.AddrPC.Offset - (uint64_t)m));
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), t->host, &f, &ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) break;
            }
        }
        fprintf(stderr, "\n");
        ResumeThread(t->host);
    }
}

/* Arrêt : suspension de tous les threads invités (sauf l'appelant) */
void svc_suspendre_invites(void) {
    for (int i = 0; i < MAX_OBJ; i++) {
        if (g_obj[i].type != OBJ_THREAD || !g_obj[i].thread) continue;
        Thread *t = g_obj[i].thread;
        if (t->exited || !t->host || GetThreadId(t->host) == GetCurrentThreadId()) continue;
        SuspendThread(t->host);
    }
}

/* état de tous les threads invités (appel système en cours, arguments, adresses de pile) */
void svc_vider_threads(void) {
    fprintf(stderr, "[threads] ---- état à t=%.0f s\n", GetTickCount64() / 1000.0);
    for (int i = 0; i < MAX_OBJ; i++) {
        if (g_obj[i].type != OBJ_THREAD || !g_obj[i].thread) continue;
        Thread *t = g_obj[i].thread;
        if (t->exited) continue;
        uint32_t sp = t->cpu.r[13];
        fprintf(stderr, "[threads] %08X %-12s prio %2d cœur %d tenu %d attente %d @%08X svc %03X lr %08X args %08X %08X %08X %08X | sp %08X pile",
                t->handle, g_obj[i].name, t->priority, t->core, t->coeur_tenu, t->waiting_kind, t->wait_addr, t->svc_en_cours, t->svc_lr,
                t->svc_args[0], t->svc_args[1], t->svc_args[2], t->svc_args[3], sp);
        MEMORY_BASIC_INFORMATION mbi;
        int lisible = sp >= 0x10000 && VirtualQuery(g_mem + sp, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
                      (uint8_t *)mbi.BaseAddress + mbi.RegionSize >= g_mem + sp + 96;
        for (int k = 0; k < 24 && lisible; k++) {
            uint32_t v = *(uint32_t *)(g_mem + sp + 4 * k);
            if (v >= 0x200000 && v < 0x2000000) fprintf(stderr, " %08X", v); /* adresses de code probables */
        }
        if (t->svc_en_cours == 0x118 && t->svc_args[1] >= 0x10000 && t->svc_args[2] >= 1 && t->svc_args[2] <= 64) {
            fprintf(stderr, " | handles");
            for (uint32_t k = 0; k < t->svc_args[2] && k < 8; k++) { /* objets attendus (WaitSynchronization) */
                uint32_t h = rd32(t->svc_args[1] + 4 * k);
                Obj *o = get_obj(h);
                fprintf(stderr, " %08X(%s)", h, o ? o->name : "?");
            }
        }
        fprintf(stderr, "\n");
    }
}

/* diagnostic : état de tous les threads toutes les N secondes (NX_DUMP_THREADS=N) */
static DWORD WINAPI dump_threads(LPVOID p) {
    DWORD periode = (DWORD)(uintptr_t)p;
    for (;;) {
        Sleep(periode * 1000);
        fflush(stderr);
        svc_vider_threads();
        jeu_bilan();
        if (getenv("NX_SCAN_VPTR")) { /* objets dont le vptr précède (≤ 0x400 octets) une entrée de vtable donnée */
            uint32_t cible = (uint32_t)strtoul(getenv("NX_SCAN_VPTR"), NULL, 16);
            uint32_t vus[16] = {0}, exemple[16] = {0};
            long nb[16] = {0};
            int nv = 0;
            for (uint64_t a = 0x10000; a < 0xE0000000ull;) {
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQuery(g_mem + a, &mbi, sizeof mbi)) break;
                uint64_t fin = (uint64_t)((uint8_t *)mbi.BaseAddress - g_mem) + mbi.RegionSize;
                if (mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_READWRITE))
                    for (uint64_t x = a; x + 4 <= fin; x += 4) {
                        uint32_t v = *(uint32_t *)(g_mem + x);
                        if (v <= cible && v + 0x400 > cible && !(v & 3)) {
                            int i;
                            for (i = 0; i < nv && vus[i] != v; i++) {}
                            if (i == nv && nv < 16) { vus[nv] = v; exemple[nv] = (uint32_t)x; nv++; }
                            if (i < 16) nb[i]++;
                        }
                    }
                a = fin;
            }
            fprintf(stderr, "[vptr] cible %08X :", cible);
            for (int i = 0; i < nv; i++) fprintf(stderr, " %08X(x%ld, ex %08X)", vus[i], nb[i], exemple[i]);
            fprintf(stderr, "\n");
        }
        if (getenv("NX_SCAN_NAN")) { /* diagnostic : NaN « x86 » (0xFFC00000) et ARM (0x7FC00000) dans la mémoire invitée */
            uint64_t n_x86 = 0, n_arm = 0;
            uint32_t premiers[16];
            int np = 0;
            for (uint64_t a = 0x10000; a < 0xF0000000ull;) {
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQuery(g_mem + a, &mbi, sizeof mbi)) break;
                uint64_t fin = (uint64_t)((uint8_t *)mbi.BaseAddress - g_mem) + mbi.RegionSize;
                if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE))) {
                    for (uint64_t x = a; x + 4 <= fin; x += 4) {
                        uint32_t v = *(uint32_t *)(g_mem + x);
                        if (v == 0xFFC00000u) {
                            n_x86++;
                            if (np < 16 && (np == 0 || x - premiers[np - 1] > 0x1000)) premiers[np++] = (uint32_t)x;
                        } else if (v == 0x7FC00000u)
                            n_arm++;
                    }
                }
                a = fin;
            }
            fprintf(stderr, "[nan] 0xFFC00000 : %llu, 0x7FC00000 : %llu ; zones :", (unsigned long long)n_x86, (unsigned long long)n_arm);
            for (int i = 0; i < np; i++) fprintf(stderr, " %08X", premiers[i]);
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }
    return 0;
}

void svc_init(void) {
    if (getenv("NX_DUMP_THREADS")) CreateThread(NULL, 0, dump_threads, (LPVOID)(uintptr_t)atoi(getenv("NX_DUMP_THREADS")), 0, NULL);
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_sched);
    InitializeConditionVariable(&g_sched_cv);
    g_tls_slot = TlsAlloc();
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_qpc0);
    rt_commit(TLS_BASE, TLS_SIZE);
    ipc_init();
}

/* Le thread principal est créé par rt.c avec son Cpu : on l'enregistre comme objet Thread. */
Cpu *svc_adopt_main(Cpu *c) {
    Thread *t = calloc(1, sizeof *t);
    t->cpu = *c;
    InitializeConditionVariable(&t->cv);
    t->priority = 44;
    t->core = 0;
    t->coeur_tenu = -1;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->host, 0, FALSE, DUPLICATE_SAME_ACCESS);
    uint32_t h = new_handle(OBJ_THREAD, t, "main");
    t->handle = h;
    TlsSetValue(g_tls_slot, t);
    coeur_prendre(t);
    return &t->cpu;
}

/* ------------------------------------------------------------------ répartition */
/* Mappages svcMapMemory (pile des threads, etc.) : sémantique par copie, la source étant inaccessible
 * côté Horizon tant que la destination est mappée. */
#define MAX_MAPS 256
static struct { uint32_t dst, src, size; } g_maps[MAX_MAPS];

static void query_memory(uint32_t out, uint32_t addr) {
    /* régions connues : image (code/données), pile principale, TLS, tas, mappages ; le reste est libre */
    struct { uint32_t b, s, state, perm; } R[4 + MAX_MAPS] = {
        {rt_image_base, rt_image_taille, 0x03, 5}, /* code (état Code) */
        {MAIN_STACK_BASE, MAIN_STACK_SIZE, 0x0B, 3},
        {TLS_BASE, TLS_SIZE, 0x0C, 3},
        {HEAP_BASE, g_heap_size, 0x05, 3},
    };
    for (int i = 0; i < MAX_MAPS; i++)
        if (g_maps[i].size) { R[4 + i].b = g_maps[i].dst; R[4 + i].s = g_maps[i].size; R[4 + i].state = 0x0B; R[4 + i].perm = 3; }
    uint32_t base = 0, size = 0, state = 0, perm = 0;
    for (unsigned i = 0; i < sizeof R / sizeof R[0]; i++)
        if (R[i].s && addr >= R[i].b && addr - R[i].b < R[i].s) { base = R[i].b; size = R[i].s; state = R[i].state; perm = R[i].perm; }
    if (!size) { /* trou : de la fin de la région précédente au début de la suivante */
        uint32_t lo = 0, hi = 0xFFFFFFFFu;
        for (unsigned i = 0; i < sizeof R / sizeof R[0]; i++) {
            if (!R[i].s) continue;
            if (R[i].b + R[i].s <= addr && R[i].b + R[i].s > lo) lo = R[i].b + R[i].s;
            if (R[i].b > addr && R[i].b < hi) hi = R[i].b;
        }
        base = lo; size = hi - lo;
    }
    wr64(out, base); wr64(out + 8, size);
    wr32(out + 16, state); wr32(out + 20, 0); wr32(out + 24, perm);
    wr32(out + 28, 0); wr32(out + 32, 0); wr32(out + 36, 0);
}

static uint64_t get_info(uint32_t type, uint32_t handle, uint64_t sub, uint32_t *res) {
    *res = RES_OK;
    switch (type) {
    case 0: return 0xF;                 /* CoreMask */
    case 1: return 0xFFFFFFFFFFFFFFFFull; /* PriorityMask */
    case 2: return ALIAS_BASE;
    case 3: return ALIAS_SIZE;
    case 4: return HEAP_BASE;
    case 5: return HEAP_MAX;
    case 6: return 0xCD500000ull;       /* mémoire totale (3,2 Gio) */
    case 7: return g_heap_size + 0x02200000ull;
    case 8: return 0;
    case 9: return 0;
    case 11: return ((uint64_t)rand() << 32) ^ (uint64_t)rand() ^ ticks();
    case 12: return 0x00200000ull;      /* AslrRegion */
    case 13: return 0xFFE00000ull;
    case 14: return STACK_REGION_BASE;
    case 15: return STACK_REGION_SIZE;
    case 16: case 17: return 0;
    case 18: return jeu_title_id(); /* ProgramId */
    case 20: return 0;
    case 21: return 0xCD500000ull;
    case 22: return g_heap_size + 0x02200000ull;
    case 23: return 1;
    case 25: return ticks();
    }
    fprintf(stderr, "[svc] GetInfo type %u sub %llu non géré\n", type, (unsigned long long)sub);
    *res = 0xF001u;
    return 0;
}

static void svc_dispatch(Cpu *c, uint32_t imm) {
    Thread *t = cur_thread();
    uint32_t *r = c->r;
    switch (imm) {
    case 0x01: { /* SetHeapSize(size) -> addr */
        uint32_t size = r[1];
        if (size > HEAP_MAX) { r[0] = 0xD001u; return; }
        if (size > g_heap_size) rt_commit(HEAP_BASE + g_heap_size, size - g_heap_size);
        g_heap_size = size;
        r[0] = RES_OK; r[1] = HEAP_BASE;
        return;
    }
    case 0x04: { /* MapMemory(dst, src, size) */
        uint32_t dst = r[0], src = r[1], size = r[2];
        if (getenv("NX_TRACE_MAP")) fprintf(stderr, "[mem] MapMemory %08X <- %08X (%X octets) lr=%08X\n", dst, src, size, c->r[14]);
        EnterCriticalSection(&g_lock);
        int i = 0;
        while (i < MAX_MAPS && g_maps[i].size) i++;
        if (i == MAX_MAPS) rt_fatal(c, "trop de mappages MapMemory");
        rt_commit(dst, size);
        memcpy(g_mem + dst, g_mem + src, size);
        g_maps[i].dst = dst; g_maps[i].src = src; g_maps[i].size = size;
        LeaveCriticalSection(&g_lock);
        r[0] = RES_OK;
        return;
    }
    case 0x05: { /* UnmapMemory(dst, src, size) */
        uint32_t dst = r[0], src = r[1], size = r[2];
        if (getenv("NX_TRACE_MAP")) fprintf(stderr, "[mem] UnmapMemory %08X -> %08X (%X octets) lr=%08X\n", dst, src, size, c->r[14]);
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < MAX_MAPS; i++)
            if (g_maps[i].size && g_maps[i].dst == dst) {
                memcpy(g_mem + src, g_mem + dst, size);
                if (!VirtualFree(g_mem + dst, size, MEM_DECOMMIT))
                    fprintf(stderr, "[mem] ERREUR UnmapMemory %08X (%X octets) : %lu\n", dst, size, GetLastError());
                g_maps[i].size = 0;
            }
        LeaveCriticalSection(&g_lock);
        r[0] = RES_OK;
        return;
    }
    case 0x03: /* SetMemoryAttribute */
        r[0] = RES_OK; return;
    case 0x06: query_memory(r[0], r[2]); r[0] = RES_OK; r[1] = 0; return;
    case 0x07: ExitProcess(0);
    case 0x08: { /* CreateThread(priority=r0, func=r1, arg=r2, stack=r3, core=r4) */
        Thread *n = calloc(1, sizeof *n);
        InitializeConditionVariable(&n->cv);
        n->entry = r[1]; n->arg = r[2]; n->priority = (int32_t)r[0];
        n->core = (int32_t)r[4] >= 0 ? (int32_t)r[4] : 0;
        n->coeur_tenu = -1;
        n->cpu.r[0] = r[2]; n->cpu.r[13] = r[3];
        n->cpu.tpidruro = svc_alloc_tls(&n->cpu);
        EnterCriticalSection(&g_lock);
        n->handle = new_handle(OBJ_THREAD, n, "thread");
        LeaveCriticalSection(&g_lock);
        r[0] = RES_OK; r[1] = n->handle;
        return;
    }
    case 0x09: { /* StartThread */
        Thread *n = thread_by_handle(r[0]);
        if (!n) { r[0] = RES_INVALID_HANDLE; return; }
        n->host = CreateThread(NULL, 64u << 20, thread_main, n, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        r[0] = RES_OK;
        return;
    }
    case 0x0A: /* ExitThread */
        coeur_rendre(t);
        EnterCriticalSection(&g_lock);
        if (t) t->exited = 1;
        wake_all_sync();
        LeaveCriticalSection(&g_lock);
        ExitThread(0);
    case 0x0B: { /* SleepThread(ns) */
        int64_t ns = (int64_t)(((uint64_t)r[1] << 32) | r[0]);
        if (ns > 0 || coeur_convoite(t)) { coeur_rendre(t); t->a_reprendre = 1; } /* yield : céder seulement si attendu */
        if (ns <= 0) SwitchToThread();
        else Sleep((DWORD)((ns + 999999) / 1000000));
        return;
    }
    case 0x0C: { /* GetThreadPriority(handle=r1) */
        Thread *n = thread_by_handle(r[1]);
        r[0] = n ? RES_OK : RES_INVALID_HANDLE; r[1] = n ? (uint32_t)n->priority : 0;
        return;
    }
    case 0x0D: { /* SetThreadPriority(handle=r0, prio=r1) */
        Thread *n = thread_by_handle(r[0]);
        if (n) n->priority = (int32_t)r[1];
        r[0] = n ? RES_OK : RES_INVALID_HANDLE;
        return;
    }
    case 0x0E: r[0] = RES_OK; r[1] = 0; r[2] = 0xF; r[3] = 0; return; /* GetThreadCoreMask */
    case 0x0F: { /* SetThreadCoreMask(handle, cœur idéal, masque) : le rendu/reprise du jeton est fait par rt_svc */
        Thread *n = thread_by_handle(r[0]);
        int32_t ideal = (int32_t)r[1];
        uint32_t masque = r[2];
        if (n) {
            if (ideal >= 0) n->core = ideal;
            else if (masque && !(masque & (1u << n->core))) for (int k = 0; k < NB_COEURS; k++) if (masque & (1u << k)) { n->core = k; break; }
        }
        r[0] = n ? RES_OK : RES_INVALID_HANDLE;
        return;
    }
    case 0x10: r[0] = t ? (uint32_t)coeur_de(t) : 0; return; /* cœur invité (et non celui du PC) */
    case 0x16: { /* CloseHandle */
        Obj *o = get_obj(r[0]);
        if (o && r[0] != PSEUDO_THREAD) o->type = OBJ_FREE;
        r[0] = RES_OK;
        return;
    }
    case 0x18: { /* WaitSynchronization(handles=r1, n=r2, timeout=r0:r3) */
        int64_t to = (int64_t)(((uint64_t)r[3] << 32) | r[0]);
        int32_t n = (int32_t)r[2];
        uint32_t hs[64];
        if (n < 0 || n > 64) { r[0] = RES_OUT_OF_RANGE; return; }
        for (int i = 0; i < n; i++) hs[i] = rd32(r[1] + 4 * i);
        EnterCriticalSection(&g_lock);
        for (;;) {
            if (t->annule) { t->annule = 0; LeaveCriticalSection(&g_lock); r[0] = RES_CANCELLED; return; }
            for (int i = 0; i < n; i++) {
                Obj *o = get_obj(hs[i]);
                if (!o) {
                    static int nj = 0; /* diagnostic : attente sur un handle inconnu (boucle active côté SDK) */
                    if (nj++ < 8) fprintf(stderr, "[svc] WaitSynchronization : handle %08X inconnu (thread %08X, lr=%08X)\n", hs[i],
                                          t ? t->handle : 0, c->r[14]);
                    LeaveCriticalSection(&g_lock); r[0] = RES_INVALID_HANDLE; return;
                }
                int sig = 0;
                if (o->type == OBJ_EVENT_R || o->type == OBJ_EVENT_W) sig = o->event->signaled;
                else if (o->type == OBJ_THREAD) sig = o->thread->exited;
                else rt_fatal(c, "WaitSynchronization sur un objet de type %d (%s)", o->type, o->name);
                if (sig) { LeaveCriticalSection(&g_lock); r[0] = RES_OK; r[1] = (uint32_t)i; return; }
            }
            if (to == 0) break;
            t->waiting_kind = 4;
            wait_list_add(t);
            int expire = block(t, to);
            if (t->waiting_kind == 4) wait_list_remove(t);
            t->waiting_kind = 0;
            if (expire) break;
        }
        LeaveCriticalSection(&g_lock);
        r[0] = RES_TIMEOUT;
        return;
    }
    case 0x11: case 0x12: case 0x17: { /* SignalEvent / ClearEvent / ResetSignal */
        EnterCriticalSection(&g_lock);
        Obj *o = get_obj(r[0]);
        if (!o || (o->type != OBJ_EVENT_R && o->type != OBJ_EVENT_W)) { LeaveCriticalSection(&g_lock); r[0] = RES_INVALID_HANDLE; return; }
        if (imm == 0x11) { o->event->signaled = 1; wake_all_sync(); }
        else {
            uint32_t was = o->event->signaled;
            o->event->signaled = 0;
            if (imm == 0x17 && !was) { LeaveCriticalSection(&g_lock); r[0] = RES_INVALID_STATE; return; }
        }
        LeaveCriticalSection(&g_lock);
        r[0] = RES_OK;
        return;
    }
    case 0x13: { /* MapSharedMemory(handle, addr, size, perm) */
        Obj *o = get_obj(r[0]);
        if (!o || o->type != OBJ_SHMEM) { r[0] = RES_INVALID_HANDLE; return; }
        ShMem *m = (ShMem *)o->p;
        rt_commit(r[1], r[2]);
        if (m->init) memcpy(g_mem + r[1], m->init, r[2] < m->size ? r[2] : m->size);
        else memset(g_mem + r[1], 0, r[2]);
        m->addr = r[1];
        r[0] = RES_OK;
        return;
    }
    case 0x14: r[0] = RES_OK; return; /* UnmapSharedMemory */
    case 0x2A: case 0x2B: case 0x2C: case 0x5D: case 0x5E: case 0x5F: /* vidages de cache : sans objet sur PC */
        r[0] = RES_OK;
        return;
    case 0x15: { /* CreateTransferMemory(addr=r1, size=r2, perm=r3) -> handle r1 : la mémoire reste en place */
        EnterCriticalSection(&g_lock);
        uint32_t h = new_handle(OBJ_OTHER, NULL, "tmem");
        LeaveCriticalSection(&g_lock);
        r[0] = RES_OK; r[1] = h;
        return;
    }
    case 0x45: { /* CreateEvent -> r1 écriture, r2 lecture */
        uint32_t rh;
        uint32_t wh = svc_new_event(0, &rh);
        r[0] = RES_OK; r[1] = wh; r[2] = rh;
        return;
    }
    case 0x19: { /* CancelSynchronization(thread) : l'attente en cours (ou la prochaine) se termine par « annulé » */
        EnterCriticalSection(&g_lock);
        Obj *o = get_obj(r[0]);
        if (!o || o->type != OBJ_THREAD) { LeaveCriticalSection(&g_lock); r[0] = RES_INVALID_HANDLE; return; }
        o->thread->annule = 1;
        if (o->thread->waiting_kind == 4) wake(o->thread);
        LeaveCriticalSection(&g_lock);
        r[0] = RES_OK;
        return;
    }
    case 0x1A: r[0] = arbitrate_lock(t, r[0], r[1], r[2]); return;
    case 0x1B: /* ArbitrateUnlock(addr) */
        EnterCriticalSection(&g_lock);
        mutex_release(r[0]);
        LeaveCriticalSection(&g_lock);
        r[0] = RES_OK;
        return;
    case 0x1C: { /* WaitProcessWideKeyAtomic(addr, key, tag, timeout=r3:r4) */
        uint32_t addr = r[0], key = r[1], tag = r[2];
        int64_t to = (int64_t)(((uint64_t)r[4] << 32) | r[3]);
        EnterCriticalSection(&g_lock);
        /* comme le noyau de la console : publier la clé et s'inscrire AVANT de libérer le mutex. Sinon, un autre thread
         * peut prendre le mutex, voir la clé à 0 (« personne n'attend »), ne pas signaler, et ce réveil est perdu :
         * le thread reste bloqué indéfiniment (blocages sporadiques, travaux de rendu jamais exécutés). */
        k_store(key, 1);
        t->waiting_kind = 2; t->wait_key = key; t->wait_addr = addr;
        wait_list_add(t);
        t->result = RES_OK;
        mutex_release(addr);
        if (block(t, to)) { /* délai écoulé : sortir de la file de la clé */
            if (t->waiting_kind == 2) { wait_list_remove(t); t->waiting_kind = 0; }
            t->result = RES_TIMEOUT;
        }
        /* réacquisition du mutex */
        uint32_t res = t->result;
        for (;;) {
            uint32_t h_ = k_verrou(addr);
            uint32_t v = rd32(addr);
            if (v == 0) { wr32(addr, tag); k_deverrou(h_); break; }
            if (v == tag || (v & ~WAITER_BIT) == tag) { k_deverrou(h_); break; }
            wr32(addr, v | WAITER_BIT);
            k_deverrou(h_);
            t->waiting_kind = 1; t->wait_addr = addr;
            wait_list_add(t);
            block(t, -1);
            break; /* mutex_release nous l'a transmis */
        }
        LeaveCriticalSection(&g_lock);
        r[0] = res;
        return;
    }
    case 0x1D: { /* SignalProcessWideKey(key, count) */
        uint32_t key = r[0];
        int32_t count = (int32_t)r[1];
        EnterCriticalSection(&g_lock);
        int woken = 0, restants = 0;
        for (;;) {
            Thread *best = NULL;
            for (Thread *w = g_waiters; w; w = w->next_wait)
                if (w->waiting_kind == 2 && w->wait_key == key && (!best || w->priority < best->priority)) best = w;
            if (!best || (count > 0 && woken >= count)) break;
            wait_list_remove(best);
            best->waiting_kind = 0;
            wake(best);
            woken++;
        }
        for (Thread *w = g_waiters; w; w = w->next_wait)
            if (w->waiting_kind == 2 && w->wait_key == key) restants++;
        if (!restants) k_store(key, 0);
        LeaveCriticalSection(&g_lock);
        return;
    }
    case 0x1E: { uint64_t tk = ticks(); r[0] = (uint32_t)tk; r[1] = (uint32_t)(tk >> 32); return; }
    case 0x1F: { /* ConnectToNamedPort(name=r1) */
        char name[13] = {0};
        memcpy(name, g_mem + r[1], 12);
        r[1] = ipc_connect(name);
        r[0] = RES_OK;
        return;
    }
    case 0x21: /* SendSyncRequest */
        r[0] = ipc_dispatch(c, r[0]);
        return;
    case 0x25: { /* GetThreadId(handle=r1) */
        Thread *n = thread_by_handle(r[1]);
        r[0] = n ? RES_OK : RES_INVALID_HANDLE; r[1] = n ? (n->handle & 0xFFFF) : 0; r[2] = 0;
        return;
    }
    case 0x26: /* Break */
        rt_fatal(c, "svcBreak(raison=%08X, arg=%08X, taille=%08X)", r[0], r[1], r[2]);
    case 0x27: { /* OutputDebugString(str, len) */
        fprintf(stderr, "[debug] %.*s\n", (int)r[1], (const char *)(g_mem + r[0]));
        r[0] = RES_OK;
        return;
    }
    case 0x29: { /* GetInfo(type=r1, handle=r2, sub=r0|r3<<32) */
        uint32_t res;
        uint64_t v = get_info(r[1], r[2], ((uint64_t)r[3] << 32) | r[0], &res);
        r[0] = res; r[1] = (uint32_t)v; r[2] = (uint32_t)(v >> 32);
        return;
    }
    case 0x34: { /* WaitForAddress(addr, type, value, timeout=r3:r4) */
        uint32_t addr = r[0], type = r[1];
        int32_t value = (int32_t)r[2];
        int64_t to = (int64_t)(((uint64_t)r[4] << 32) | r[3]);
        EnterCriticalSection(&g_lock);
        uint32_t h_ = k_verrou(addr); /* lecture-modification-écriture atomique vis-à-vis de ldrex/strex */
        int32_t cur = (int32_t)rd32(addr);
        int attendre = 0;
        if (type == 0) attendre = cur < value;
        else if (type == 1) { attendre = cur < value; if (attendre) __atomic_store_n((uint32_t *)HP(addr), (uint32_t)(cur - 1), __ATOMIC_RELEASE); }
        else if (type == 2) attendre = cur == value;
        k_deverrou(h_);
        if (!attendre) { LeaveCriticalSection(&g_lock); r[0] = RES_INVALID_STATE; return; }
        if (to == 0) { LeaveCriticalSection(&g_lock); r[0] = RES_TIMEOUT; return; }
        t->waiting_kind = 3; t->wait_addr = addr;
        wait_list_add(t);
        t->result = RES_OK;
        if (block(t, to)) {
            if (t->waiting_kind == 3) { wait_list_remove(t); t->waiting_kind = 0; }
            t->result = RES_TIMEOUT;
        }
        LeaveCriticalSection(&g_lock);
        r[0] = t->result;
        return;
    }
    case 0x35: { /* SignalToAddress(addr, type, value, count) */
        uint32_t addr = r[0], type = r[1];
        int32_t value = (int32_t)r[2], count = (int32_t)r[3];
        EnterCriticalSection(&g_lock);
        int nb = 0;
        for (Thread *w = g_waiters; w; w = w->next_wait)
            if (w->waiting_kind == 3 && w->wait_addr == addr) nb++;
        if (type == 1 || type == 2) { /* comparaison et modification atomiques vis-à-vis de ldrex/strex */
            uint32_t h_ = k_verrou(addr);
            if ((int32_t)rd32(addr) != value) { k_deverrou(h_); LeaveCriticalSection(&g_lock); r[0] = RES_INVALID_STATE; return; }
            int32_t nv = value + 1;
            if (type == 2) {
                if (count <= 0) nv = nb > 0 ? value - 1 : value + 1;
                else nv = nb == 0 ? value + 1 : (nb <= count ? value - 1 : value);
            }
            __atomic_store_n((uint32_t *)HP(addr), (uint32_t)nv, __ATOMIC_RELEASE);
            k_deverrou(h_);
        }
        int woken = 0;
        for (;;) {
            Thread *best = NULL;
            for (Thread *w = g_waiters; w; w = w->next_wait)
                if (w->waiting_kind == 3 && w->wait_addr == addr && (!best || w->priority < best->priority)) best = w;
            if (!best || (count > 0 && woken >= count)) break;
            wait_list_remove(best);
            best->waiting_kind = 0;
            wake(best);
            woken++;
        }
        LeaveCriticalSection(&g_lock);
        r[0] = RES_OK;
        return;
    }
    }
    rt_fatal(c, "svc 0x%02X non implémenté", imm);
}

static void rt_svc_corps(Cpu *c, uint32_t imm);

/* Statistiques de durée des SVC (NX_SVC_STATS=s : rapport toutes les s secondes) : temps cumulé par (thread, svc,
 * adresse de retour invitée), pour savoir où les threads du jeu attendent. */
#define NB_STATS 4096
typedef struct { uint32_t thread, imm, lr, n; uint64_t ticks; } StatSvc;
static StatSvc g_stats[NB_STATS];
static CRITICAL_SECTION g_stats_cs;
static int g_stats_periode = -1;
static LARGE_INTEGER g_stats_freq;

static int cmp_stat(const void *a, const void *b) {
    uint64_t x = ((const StatSvc *)a)->ticks, y = ((const StatSvc *)b)->ticks;
    return x < y ? 1 : x > y ? -1 : 0;
}

static DWORD WINAPI stats_rapport(LPVOID arg) {
    (void)arg;
    for (;;) {
        Sleep((DWORD)g_stats_periode * 1000);
        static StatSvc copie[NB_STATS];
        EnterCriticalSection(&g_stats_cs);
        memcpy(copie, g_stats, sizeof copie);
        memset(g_stats, 0, sizeof g_stats);
        LeaveCriticalSection(&g_stats_cs);
        qsort(copie, NB_STATS, sizeof *copie, cmp_stat);
        fprintf(stderr, "[svc-stats] sur %d s (temps cumulé, appels) :\n", g_stats_periode);
        for (int i = 0; i < 30 && copie[i].n; i++)
            fprintf(stderr, "[svc-stats] thread %08X svc %02X lr=%08X : %8.1f ms, %u appels\n", copie[i].thread, copie[i].imm, copie[i].lr,
                    1000.0 * (double)copie[i].ticks / (double)g_stats_freq.QuadPart, copie[i].n);
        fflush(stderr);
    }
    return 0;
}

static void stats_svc(Thread *t, uint32_t imm, uint32_t lr, uint64_t ticks) {
    uint32_t th = t ? t->handle : 0;
    uint32_t hsh = (th * 2654435761u ^ imm * 40503u ^ lr * 2246822519u) % NB_STATS;
    EnterCriticalSection(&g_stats_cs);
    for (int k = 0; k < 64; k++) {
        StatSvc *e = &g_stats[(hsh + k) % NB_STATS];
        if (!e->n) { e->thread = th; e->imm = imm; e->lr = lr; }
        if (e->thread == th && e->imm == imm && e->lr == lr) { e->n++; e->ticks += ticks; break; }
    }
    LeaveCriticalSection(&g_stats_cs);
}

/* Point d'entrée des SVC ; NX_SVC_TRACE=1 journalise arguments et résultats. */
void rt_svc(Cpu *c, uint32_t imm) {
    if (g_stats_periode < 0) {
        g_stats_periode = getenv("NX_SVC_STATS") ? atoi(getenv("NX_SVC_STATS")) : 0;
        if (g_stats_periode > 0) {
            InitializeCriticalSection(&g_stats_cs);
            QueryPerformanceFrequency(&g_stats_freq);
            CreateThread(NULL, 0, stats_rapport, NULL, 0, NULL);
        }
    }
    LARGE_INTEGER t_debut;
    const uint32_t lr_appel = c->r[14];
    if (g_stats_periode > 0) QueryPerformanceCounter(&t_debut);
    rt_svc_corps(c, imm);
    if (g_stats_periode > 0) {
        LARGE_INTEGER t_fin;
        QueryPerformanceCounter(&t_fin);
        stats_svc(cur_thread(), imm, lr_appel, (uint64_t)(t_fin.QuadPart - t_debut.QuadPart));
    }
}

static void rt_svc_corps(Cpu *c, uint32_t imm) {
    static int trace = -1;
    if (trace < 0) trace = getenv("NX_SVC_TRACE") != NULL;
    if (trace) fprintf(stderr, "[svc] %02X (%08X %08X %08X %08X %08X)", imm, c->r[0], c->r[1], c->r[2], c->r[3], c->r[4]);
    Thread *t = cur_thread();
    if (t) { t->svc_lr = c->r[14]; for (int i = 0; i < 4; i++) t->svc_args[i] = c->r[i]; t->svc_en_cours = 0x100 | imm; }
    if ((imm == 0x0F || imm == 0x21) && t) { coeur_rendre(t); t->a_reprendre = 1; } /* changement de cœur ; IPC : le client est bloqué pendant le service */
    svc_dispatch(c, imm);
    if (t && t->a_reprendre) { t->a_reprendre = 0; coeur_prendre(t); }
    if (t) t->svc_en_cours = 0;
    if (trace) fprintf(stderr, " -> %08X %08X %08X\n", c->r[0], c->r[1], c->r[2]);
}

uint32_t svc_current_handle(void) { Thread *t = cur_thread(); return t ? t->handle : 0; }
