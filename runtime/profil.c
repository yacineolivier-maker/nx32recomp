/* Profileur par échantillonnage de tous les threads du processus (NX_PROFIL_TOUS=s : rapport toutes les s secondes).
 * Toutes les 2 ms, chaque thread est suspendu le temps de lire son RIP (aucune allocation pendant la suspension) ;
 * un RIP juste après une instruction syscall (0F 05) compte comme attente. Rapport : par thread actif, part d'activité
 * et fonctions les plus fréquentes (l'exécutable : symboles de la PDB via dbghelp ; eden_gpu.dll : fichier .map de lld,
 * NX_PROFIL_MAP, défaut build/cmake/eden_gpu.map). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILS 128
#define MAX_ECH (1 << 20)

typedef struct {
    DWORD tid;
    HANDLE h;
    uint32_t actifs, attentes;
} Fil;

typedef struct {
    DWORD tid;
    uint32_t attente; /* 1 : échantillon en attente noyau, rip = premier appelant dans l'exécutable / eden_gpu.dll */
    uint64_t rip;
} Ech;

static Fil g_fils[MAX_FILS];
static int g_nfils;
static Ech *g_ech;
static uint32_t g_nech;

/* ---- symboles de eden_gpu.dll (.map de lld-link : « Publics by Value ») */
typedef struct {
    uint32_t rva;
    char *nom;
} SymMap;
static SymMap *g_map;
static int g_nmap;
static uint64_t g_base_eden;

static int cmp_map(const void *a, const void *b) {
    uint32_t x = ((const SymMap *)a)->rva, y = ((const SymMap *)b)->rva;
    return x < y ? -1 : x > y;
}

static void charger_map(void) {
    const char *chemin = getenv("NX_PROFIL_MAP") ? getenv("NX_PROFIL_MAP") : "build/cmake/eden_gpu.map";
    FILE *f = fopen(chemin, "r");
    if (!f) return;
    char ligne[4096];
    int cap = 0;
    while (fgets(ligne, sizeof ligne, f)) {
        unsigned sec, off;
        char nom[3000];
        unsigned long long va;
        if (sscanf(ligne, " %x:%x %2999s %llx", &sec, &off, nom, &va) != 4 || sec == 0 || va < 0x180000000ull) continue;
        if (g_nmap == cap) {
            cap = cap ? cap * 2 : 65536;
            g_map = (SymMap *)realloc(g_map, cap * sizeof *g_map);
        }
        g_map[g_nmap].rva = (uint32_t)(va - 0x180000000ull);
        g_map[g_nmap].nom = _strdup(nom);
        g_nmap++;
    }
    fclose(f);
    qsort(g_map, g_nmap, sizeof *g_map, cmp_map);
}

static const char *sym_eden(uint32_t rva) {
    int lo = 0, hi = g_nmap - 1, r = -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (g_map[m].rva <= rva) { r = m; lo = m + 1; }
        else hi = m - 1;
    }
    return r < 0 ? NULL : g_map[r].nom;
}

/* nom lisible d'une adresse : fonction (démanglée) ou module+décalage */
static void nommer(uint64_t rip, char *sortie, size_t n) {
    HMODULE mod = NULL;
    char chemin[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &mod))
        GetModuleFileNameA(mod, chemin, sizeof chemin);
    const char *court = strrchr(chemin, '\\') ? strrchr(chemin, '\\') + 1 : chemin;
    if (mod && (uint64_t)mod == g_base_eden && g_nmap) {
        const char *s = sym_eden((uint32_t)(rip - (uint64_t)mod));
        if (s) {
            char lisible[512];
            if (UnDecorateSymbolName(s, lisible, sizeof lisible, UNDNAME_NAME_ONLY)) snprintf(sortie, n, "eden:%s", lisible);
            else snprintf(sortie, n, "eden:%s", s);
            return;
        }
    }
    char tampon[sizeof(SYMBOL_INFO) + 512];
    SYMBOL_INFO *si = (SYMBOL_INFO *)tampon;
    memset(tampon, 0, sizeof tampon);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 511;
    DWORD64 dep = 0;
    if (SymFromAddr(GetCurrentProcess(), rip, &dep, si)) snprintf(sortie, n, "%s!%s", court, si->Name);
    else snprintf(sortie, n, "%s+0x%llX", court, (unsigned long long)(rip - (uint64_t)mod));
}

/* pile du thread suspendu (déroulement x64 par les tables d'exceptions) : premier appelant dans l'exécutable ou
 * eden_gpu.dll, 0 si introuvable ; aucune allocation (le thread suspendu peut détenir le verrou du tas) */
static uint64_t g_exe_debut, g_exe_fin, g_eden_fin;
static uint64_t site_attente(const CONTEXT *ctx) {
    CONTEXT c = *ctx;
    for (int n = 0; n < 24; n++) {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &base, NULL);
        if (!fn) { /* fonction feuille : adresse de retour au sommet de la pile */
            c.Rip = *(DWORD64 *)c.Rsp;
            c.Rsp += 8;
        } else {
            PVOID donnees = NULL;
            DWORD64 cadre = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fn, &c, &donnees, &cadre, NULL);
        }
        if (!c.Rip) return 0;
        if ((c.Rip >= g_exe_debut && c.Rip < g_exe_fin) || (g_base_eden && c.Rip >= g_base_eden && c.Rip < g_eden_fin)) return c.Rip;
    }
    return 0;
}

static void rafraichir_fils(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    DWORD pid = GetCurrentProcessId(), moi = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == moi) continue;
        int connu = 0;
        for (int i = 0; i < g_nfils; i++) connu |= g_fils[i].tid == te.th32ThreadID;
        if (connu || g_nfils == MAX_FILS) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        g_fils[g_nfils].tid = te.th32ThreadID;
        g_fils[g_nfils].h = h;
        g_fils[g_nfils].actifs = g_fils[g_nfils].attentes = 0;
        g_nfils++;
    }
    CloseHandle(snap);
}

typedef struct {
    char nom[600];
    uint32_t n;
} Ligne;

static int cmp_ligne(const void *a, const void *b) { return (int)((const Ligne *)b)->n - (int)((const Ligne *)a)->n; }
static int cmp_ech(const void *a, const void *b) {
    const Ech *x = (const Ech *)a, *y = (const Ech *)b;
    if (x->tid != y->tid) return x->tid < y->tid ? -1 : 1;
    return x->rip < y->rip ? -1 : x->rip > y->rip;
}

static void rapport(void) {
    qsort(g_ech, g_nech, sizeof *g_ech, cmp_ech);
    for (int i = 0; i < g_nfils; i++) {
        Fil *f = &g_fils[i];
        uint32_t total = f->actifs + f->attentes;
        if (!total || f->actifs * 100 < total * 5) continue; /* threads actifs à moins de 5 % : ignorés */
        char desc[128] = "";
        PWSTR w = NULL;
        typedef HRESULT(WINAPI * DescFn)(HANDLE, PWSTR *);
        static DescFn get_desc = NULL;
        if (!get_desc) get_desc = (DescFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
        if (get_desc && SUCCEEDED(get_desc(f->h, &w)) && w) {
            WideCharToMultiByte(CP_UTF8, 0, w, -1, desc, sizeof desc, NULL, NULL);
            LocalFree(w);
        }
        fprintf(stderr, "[profil] thread %lu « %s » : actif %u %% (%u échantillons)\n", f->tid, desc, f->actifs * 100 / total, total);
        /* regroupement par fonction : activité, puis sites d'attente */
        for (int liste = 0; liste < 2; liste++) {
        static Ligne lignes[4096];
        int nl = 0;
        if (liste == 1) fprintf(stderr, "[profil]   — attentes (%u %%), par appelant :\n", f->attentes * 100 / total);
        for (uint32_t k = 0; k < g_nech; k++) {
            if (g_ech[k].tid != f->tid || (int)g_ech[k].attente != liste) continue;
            char nom[600];
            nommer(g_ech[k].rip, nom, sizeof nom);
            int j = 0;
            while (j < nl && strcmp(lignes[j].nom, nom) != 0) j++;
            if (j == nl) {
                if (nl == 4096) continue;
                strcpy(lignes[nl].nom, nom);
                lignes[nl].n = 0;
                nl++;
            }
            lignes[j].n++;
        }
        qsort(lignes, nl, sizeof *lignes, cmp_ligne);
        for (int j = 0; j < nl && j < (liste ? 10 : 14); j++) fprintf(stderr, "[profil]   %5.1f %%  %.200s\n", 100.0 * lignes[j].n / total, lignes[j].nom);
        }
        f->actifs = f->attentes = 0;
    }
    for (int i = 0; i < g_nfils; i++) g_fils[i].actifs = g_fils[i].attentes = 0;
    g_nech = 0;
    fflush(stderr);
}

static DWORD WINAPI profileur(LPVOID arg) {
    DWORD periode = (DWORD)(uintptr_t)arg * 1000;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    g_ech = (Ech *)malloc(MAX_ECH * sizeof *g_ech);
    DWORD t0 = GetTickCount(), t_fils = 0;
    for (;;) {
        Sleep(2);
        if (!g_exe_debut) {
            MODULEINFO mi;
            g_exe_debut = (uint64_t)GetModuleHandleA(NULL);
            g_exe_fin = GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(NULL), &mi, sizeof mi) ? g_exe_debut + mi.SizeOfImage : g_exe_debut;
        }
        if (!g_base_eden && GetModuleHandleA("eden_gpu.dll")) {
            MODULEINFO mi;
            g_base_eden = (uint64_t)GetModuleHandleA("eden_gpu.dll");
            g_eden_fin = GetModuleInformation(GetCurrentProcess(), (HMODULE)g_base_eden, &mi, sizeof mi) ? g_base_eden + mi.SizeOfImage : g_base_eden;
            charger_map();
        }
        if (GetTickCount() - t_fils > 500) { rafraichir_fils(); t_fils = GetTickCount(); }
        for (int i = 0; i < g_nfils; i++) {
            Fil *f = &g_fils[i];
            if (SuspendThread(f->h) == (DWORD)-1) continue;
            CONTEXT ctx;
            memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_FULL;
            BOOL ok = GetThreadContext(f->h, &ctx);
            uint16_t avant = 0;
            SIZE_T lus = 0;
            if (ok) ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(ctx.Rip - 2), &avant, 2, &lus);
            const int attente = ok && lus == 2 && avant == 0x050F; /* juste après « syscall » : attente noyau */
            const uint64_t site = attente ? site_attente(&ctx) : 0;
            ResumeThread(f->h);
            if (!ok) continue;
            if (attente) {
                f->attentes++;
                if (site && g_nech < MAX_ECH) { g_ech[g_nech].tid = f->tid; g_ech[g_nech].attente = 1; g_ech[g_nech].rip = site; g_nech++; }
                continue;
            }
            f->actifs++;
            if (g_nech < MAX_ECH) { g_ech[g_nech].tid = f->tid; g_ech[g_nech].attente = 0; g_ech[g_nech].rip = ctx.Rip; g_nech++; }
        }
        if (GetTickCount() - t0 >= periode) {
            rapport();
            t0 = GetTickCount();
        }
    }
    return 0;
}

void profil_demarrer(void) {
    const char *s = getenv("NX_PROFIL_TOUS");
    if (!s) return;
    DWORD periode = (DWORD)atoi(s);
    CreateThread(NULL, 0, profileur, (LPVOID)(uintptr_t)(periode ? periode : 20), 0, NULL);
}
