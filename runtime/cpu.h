/* État du processeur AArch32 invité et primitives utilisées par le code recompilé. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Cpu {
    uint32_t r[16];
    uint32_t n, z, c, v, q; /* drapeaux APSR, 0 ou 1 */
    uint32_t ge;            /* APSR.GE (4 bits) */
    union {                 /* banc VFP/NEON : s0-s31 recouvrent d0-d15, q[k] = d[2k]:d[2k+1] */
        uint8_t b[256];
        int8_t i8[256];
        uint16_t h[128];
        int16_t i16[128];
        int32_t i32[64];
        int64_t i64[32];
        uint32_t s[64];
        float f[64];
        uint64_t d[32];
        double df[32];
    } vr;
    uint32_t fpscr;        /* hors NZCV, tenus dans fn/fz/fc/fv */
    uint32_t fn, fz, fc, fv;
    uint32_t tpidruro;     /* registre TLS lu par mrc p15, 0, rX, c13, c0, 3 */
    uint32_t excl_addr;    /* moniteur exclusif (ldrex/strex) */
    uint32_t excl_valid;
    uint32_t tpidrurw;     /* registre de thread lecture/écriture (mcr/mrc p15, 0, rX, c13, c0, 2) */
    uint32_t excl_seq;     /* version de l'emplacement de réservation au moment du ldrex (anti-ABA) */
    uint64_t excl_val;     /* valeur lue par ldrex */
} Cpu;

extern uint8_t *g_mem; /* base de l'espace d'adressage invité (4 Gio réservés) */

typedef void (*GuestFn)(Cpu *);
void rt_call_indirect(Cpu *c, uint32_t target);
void rt_svc(Cpu *c, uint32_t imm);
void rt_unimpl(Cpu *c, uint32_t addr, const char *what);
void rt_undefined(Cpu *c, uint32_t addr);
uint64_t rt_ticks(void); /* compteur système 19,2 MHz */

#define HP(a) (g_mem + (uint32_t)(a))

/* Trace des entrées de fonctions (développement) : tampon circulaire par thread, affiché par rt_fatal. */
/* Préemption coopérative (modèle Horizon) : un thread plus prioritaire qui attend notre cœur lève rc_preempt ;
 * on cède le cœur à la prochaine entrée de fonction. */
extern __declspec(thread) volatile int rc_preempt;
void rt_preempt(void);
#define RC_PREEMPT() do { if (rc_preempt) rt_preempt(); } while (0)
#ifdef RC_TRACE
extern __declspec(thread) uint32_t rc_trace_buf[64];
extern __declspec(thread) uint32_t rc_trace_pos;
#define RC_ENTER(a) do { rc_trace_buf[rc_trace_pos++ & 63] = (a); RC_PREEMPT(); } while (0)
#else
#define RC_ENTER(a) RC_PREEMPT()
#endif

static inline uint32_t rd8(uint32_t a) { return *(uint8_t *)HP(a); }
static inline uint32_t rd16(uint32_t a) { uint16_t v; memcpy(&v, HP(a), 2); return v; }
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, HP(a), 4); return v; }
static inline uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, HP(a), 8); return v; }
static inline void wr8(uint32_t a, uint32_t v) { *(uint8_t *)HP(a) = (uint8_t)v; }
static inline void wr16(uint32_t a, uint32_t v) { uint16_t x = (uint16_t)v; memcpy(HP(a), &x, 2); }
static inline void wr32(uint32_t a, uint32_t v) { memcpy(HP(a), &v, 4); }
static inline void wr64(uint32_t a, uint64_t v) { memcpy(HP(a), &v, 8); }

/* ---- arithmétique et drapeaux ---- */
static inline uint32_t adc32(Cpu *c, uint32_t a, uint32_t b, uint32_t ci, int s) {
    uint64_t u = (uint64_t)a + b + ci;
    uint32_t r = (uint32_t)u;
    if (s) {
        c->n = r >> 31;
        c->z = r == 0;
        c->c = (uint32_t)(u >> 32);
        c->v = ((~(a ^ b)) & (a ^ r)) >> 31;
    }
    return r;
}
static inline uint32_t nz32(Cpu *c, uint32_t r) { c->n = r >> 31; c->z = r == 0; return r; }
static inline uint32_t ror32(uint32_t x, uint32_t n) { n &= 31; return n ? (x >> n) | (x << (32 - n)) : x; }

/* Décalages par registre (quantité = 8 bits bas), avec retenue sortante dans *co. */
static inline uint32_t sh_lsl(uint32_t x, uint32_t n, uint32_t ci, uint32_t *co) {
    n &= 0xFF;
    if (n == 0) { *co = ci; return x; }
    if (n < 32) { *co = (x >> (32 - n)) & 1; return x << n; }
    *co = n == 32 ? x & 1 : 0;
    return 0;
}
static inline uint32_t sh_lsr(uint32_t x, uint32_t n, uint32_t ci, uint32_t *co) {
    n &= 0xFF;
    if (n == 0) { *co = ci; return x; }
    if (n < 32) { *co = (x >> (n - 1)) & 1; return x >> n; }
    *co = n == 32 ? x >> 31 : 0;
    return 0;
}
static inline uint32_t sh_asr(uint32_t x, uint32_t n, uint32_t ci, uint32_t *co) {
    n &= 0xFF;
    if (n == 0) { *co = ci; return x; }
    if (n < 32) { *co = (x >> (n - 1)) & 1; return (uint32_t)((int32_t)x >> n); }
    *co = x >> 31;
    return (uint32_t)((int32_t)x >> 31);
}
static inline uint32_t sh_ror(uint32_t x, uint32_t n, uint32_t ci, uint32_t *co) {
    n &= 0xFF;
    if (n == 0) { *co = ci; return x; }
    n &= 31;
    uint32_t r = ror32(x, n);
    *co = r >> 31;
    return r;
}

/* Accès exclusifs (LL/SC émulé) : chaque emplacement de réservation (adresse hachée) a une version, incrémentée par
 * tout strex réussi. strex n'aboutit que si la version et la valeur n'ont pas changé depuis le ldrex : cela évite le
 * problème ABA d'une simple comparaison-échange (listes libres sans verrou de l'allocateur du SDK). */
extern volatile uint32_t rt_excl_seq[4096];
extern volatile long rt_excl_verrou[4096];
static inline uint32_t excl_h(uint32_t a) { return (a >> 3) & 4095; }
static inline uint32_t excl_seq_lire(uint32_t a) { return __atomic_load_n(&rt_excl_seq[excl_h(a)], __ATOMIC_ACQUIRE); }
/* strex : l'écriture est une comparaison-échange matérielle. Une écriture ordinaire d'un autre thread (str) qui
 * modifie le mot entre ldrex et strex fait échouer strex au lieu d'être écrasée (une simple vérification suivie d'une
 * écriture laissait une fenêtre où cette écriture était perdue ; sur la console elle annule la réservation). */
static inline uint32_t excl_strex(Cpu *c, uint32_t a, uint64_t v, int taille) {
    uint32_t h = excl_h(a), ok = 0;
    if (!c->excl_valid || c->excl_addr != a) return 0;
    while (__sync_lock_test_and_set(&rt_excl_verrou[h], 1)) __builtin_ia32_pause();
    if (rt_excl_seq[h] == c->excl_seq) {
        if (taille == 1) {
            uint8_t e = (uint8_t)c->excl_val;
            ok = __atomic_compare_exchange_n((uint8_t *)HP(a), &e, (uint8_t)v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        } else if (taille == 2) {
            uint16_t e = (uint16_t)c->excl_val;
            ok = __atomic_compare_exchange_n((uint16_t *)HP(a), &e, (uint16_t)v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        } else if (taille == 4) {
            uint32_t e = (uint32_t)c->excl_val;
            ok = __atomic_compare_exchange_n((uint32_t *)HP(a), &e, (uint32_t)v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        } else {
            uint64_t e = c->excl_val;
            ok = __atomic_compare_exchange_n((uint64_t *)HP(a), &e, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        }
        if (ok) rt_excl_seq[h]++;
    }
    __sync_lock_release(&rt_excl_verrou[h]);
    return ok;
}

/* Accès atomiques ARMv8 (LDA/STL/LDAEX/STLEX) et ldrex. Sur x86, une lecture « acquire » est un mov ordinaire mais
 * interdit au compilateur C de déplacer les accès suivants avant elle ; STL est traduit en rangement séquentiellement
 * cohérent (xchg) : sur ARMv8, un STL suivi d'un LDA ne peut pas être réordonné, alors que x86 laisserait la lecture
 * passer avant l'écriture (tampon d'écriture). */
static inline uint32_t lda8(uint32_t a) { return __atomic_load_n((uint8_t *)HP(a), __ATOMIC_ACQUIRE); }
static inline uint32_t lda16(uint32_t a) { return __atomic_load_n((uint16_t *)HP(a), __ATOMIC_ACQUIRE); }
static inline uint32_t lda32(uint32_t a) { return __atomic_load_n((uint32_t *)HP(a), __ATOMIC_ACQUIRE); }
static inline uint64_t lda64(uint32_t a) { return __atomic_load_n((uint64_t *)HP(a), __ATOMIC_ACQUIRE); }
static inline uint32_t ldx8(uint32_t a) { return __atomic_load_n((uint8_t *)HP(a), __ATOMIC_RELAXED); }
static inline uint32_t ldx16(uint32_t a) { return __atomic_load_n((uint16_t *)HP(a), __ATOMIC_RELAXED); }
static inline uint32_t ldx32(uint32_t a) { return __atomic_load_n((uint32_t *)HP(a), __ATOMIC_RELAXED); }
static inline uint64_t ldx64(uint32_t a) { return __atomic_load_n((uint64_t *)HP(a), __ATOMIC_RELAXED); }
static inline void stl8(uint32_t a, uint32_t v) { __atomic_store_n((uint8_t *)HP(a), (uint8_t)v, __ATOMIC_SEQ_CST); }
static inline void stl16(uint32_t a, uint32_t v) { __atomic_store_n((uint16_t *)HP(a), (uint16_t)v, __ATOMIC_SEQ_CST); }
static inline void stl32(uint32_t a, uint32_t v) { __atomic_store_n((uint32_t *)HP(a), v, __ATOMIC_SEQ_CST); }
static inline void stl64(uint32_t a, uint64_t v) { __atomic_store_n((uint64_t *)HP(a), v, __ATOMIC_SEQ_CST); }
/* YIELD / WFE : attente active ; barrière du compilateur (une boucle de scrutation ne doit pas être « optimisée ») */
static inline void rt_pause(void) { __builtin_ia32_pause(); __asm__ __volatile__("" ::: "memory"); }

static inline uint32_t clz32(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32; }
static inline uint32_t sat_s(int64_t v, int bits, Cpu *c) {
    int64_t mx = (1LL << (bits - 1)) - 1, mn = -(1LL << (bits - 1));
    if (v > mx) { c->q = 1; return (uint32_t)mx; }
    if (v < mn) { c->q = 1; return (uint32_t)mn; }
    return (uint32_t)v;
}
static inline uint32_t sat_u(int64_t v, int bits, Cpu *c) {
    int64_t mx = (1LL << bits) - 1;
    if (v > mx) { c->q = 1; return (uint32_t)mx; }
    if (v < 0) { c->q = 1; return 0; }
    return (uint32_t)v;
}

/* ---- conditions ---- */
#define C_EQ (c->z)
#define C_NE (!c->z)
#define C_CS (c->c)
#define C_CC (!c->c)
#define C_MI (c->n)
#define C_PL (!c->n)
#define C_VS (c->v)
#define C_VC (!c->v)
#define C_HI (c->c && !c->z)
#define C_LS (!c->c || c->z)
#define C_GE (c->n == c->v)
#define C_LT (c->n != c->v)
#define C_GT (!c->z && c->n == c->v)
#define C_LE (c->z || c->n != c->v)

/* ---- VFP ---- */
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint64_t d2u(double f) { uint64_t u; memcpy(&u, &f, 8); return u; }
static inline double u2d(uint64_t u) { double f; memcpy(&f, &u, 8); return f; }

/* vcmp : N Z C V selon la comparaison IEEE (non ordonné = 0011) */
static inline void fcmp(Cpu *c, double a, double b) {
    if (a != a || b != b) { c->fn = 0; c->fz = 0; c->fc = 1; c->fv = 1; }
    else if (a == b) { c->fn = 0; c->fz = 1; c->fc = 1; c->fv = 0; }
    else if (a < b) { c->fn = 1; c->fz = 0; c->fc = 0; c->fv = 0; }
    else { c->fn = 0; c->fz = 0; c->fc = 1; c->fv = 0; }
}

/* Conversions flottant -> entier avec saturation (sémantique ARM, NaN -> 0) */
static inline int32_t cvt_f_s32(double x) {
    if (x != x) return 0;
    if (x >= 2147483647.0) return 2147483647;
    if (x <= -2147483648.0) return (int32_t)0x80000000;
    return (int32_t)x;
}
static inline uint32_t cvt_f_u32(double x) {
    if (x != x || x <= 0.0) return 0;
    if (x >= 4294967295.0) return 0xFFFFFFFFu;
    return (uint32_t)x;
}

/* VMAXNM/VMINNM : un NaN silencieux est ignoré, un NaN signalant est renvoyé rendu silencieux. */
static inline int snan_f(float x) { uint32_t u = f2u(x); return (u & 0x7FC00000u) == 0x7F800000u && (u & 0x3FFFFFu); }
static inline int snan_d(double x) { uint64_t u = d2u(x); return (u & 0x7FF8000000000000ull) == 0x7FF0000000000000ull && (u & 0x7FFFFFFFFFFFFull); }
static inline float maxnm_f(float a, float b, int mx) {
    if (snan_f(a)) return u2f(f2u(a) | 0x400000u);
    if (snan_f(b)) return u2f(f2u(b) | 0x400000u);
    if (a != a) return b; if (b != b) return a;
    if (a == 0.0f && b == 0.0f) return mx ? u2f(f2u(a) & f2u(b)) : u2f(f2u(a) | f2u(b));
    return mx ? (a > b ? a : b) : (a < b ? a : b);
}
static inline double maxnm_d(double a, double b, int mx) {
    if (snan_d(a)) return u2d(d2u(a) | 0x8000000000000ull);
    if (snan_d(b)) return u2d(d2u(b) | 0x8000000000000ull);
    if (a != a) return b; if (b != b) return a;
    if (a == 0.0 && b == 0.0) return mx ? u2d(d2u(a) & d2u(b)) : u2d(d2u(a) | d2u(b));
    return mx ? (a > b ? a : b) : (a < b ? a : b);
}

/* ---- NEON : « Standard FPSCR value » (FZ=1, DN=1, arrondi au plus proche) ---- */
static inline float nf_in(float x) {
    uint32_t u = f2u(x);
    if ((u & 0x7F800000u) == 0) return u2f(u & 0x80000000u); /* sous-normal -> zéro signé */
    return x;
}
static inline float nf_out(float x) {
    uint32_t u = f2u(x);
    if ((u & 0x7F800000u) == 0x7F800000u && (u & 0x007FFFFFu)) return u2f(0x7FC00000u); /* NaN par défaut */
    if ((u & 0x7F800000u) == 0) return u2f(u & 0x80000000u);
    return x;
}
static inline float fmax_arm(float a, float b) {
    if (a != a || b != b) return u2f(0x7FC00000u);
    if (a == 0.0f && b == 0.0f) return u2f(f2u(a) & f2u(b)); /* max(+0,-0) = +0 */
    return a > b ? a : b;
}
static inline float fmin_arm(float a, float b) {
    if (a != a || b != b) return u2f(0x7FC00000u);
    if (a == 0.0f && b == 0.0f) return u2f(f2u(a) | f2u(b)); /* min(+0,-0) = -0 */
    return a < b ? a : b;
}
static inline float recps_arm(float a, float b) { /* 2 - a*b, inf*0 -> 2 */
    if ((isinf(a) && b == 0.0f) || (isinf(b) && a == 0.0f)) return 2.0f;
    return 2.0f - nf_out(a * b);
}
static inline float rsqrts_arm(float a, float b) { /* (3 - a*b) / 2, inf*0 -> 1.5 */
    if ((isinf(a) && b == 0.0f) || (isinf(b) && a == 0.0f)) return 1.5f;
    return (3.0f - nf_out(a * b)) * 0.5f;
}
static inline uint32_t recip_estimate_(uint32_t a) { /* a sur 9 bits [256,511] */
    a = a * 2 + 1;
    uint32_t b = (1u << 19) / a;
    return (b + 1) >> 1;
}
static inline uint32_t rsqrt_estimate_(uint32_t a) {
    if (a < 256) a = a * 2 + 1;
    else { a = (a >> 1) << 1; a = (a + 1) * 2; }
    uint32_t b = 512;
    while ((uint64_t)a * (b + 1) * (b + 1) < (1ull << 28)) b++;
    return (b + 1) / 2;
}
static inline float recpe_arm(float x) {
    uint32_t u = f2u(x), sign = u & 0x80000000u, exp = (u >> 23) & 0xFF, frac = u & 0x7FFFFFu;
    if (exp == 0xFF) return frac ? u2f(0x7FC00000u) : u2f(sign);
    if (exp == 0) return u2f(sign | 0x7F800000u);
    if (exp >= 253) return u2f(sign); /* |x| >= 2^126 : résultat sous-normal -> zéro (FZ) */
    uint32_t scaled = 256 | (frac >> 15);
    uint32_t est = recip_estimate_(scaled);
    int32_t rexp = 253 - (int32_t)exp;
    return u2f(sign | ((uint32_t)rexp << 23) | ((est & 0xFF) << 15));
}
static inline float rsqrte_arm(float x) {
    uint32_t u = f2u(x), sign = u & 0x80000000u, exp = (u >> 23) & 0xFF, frac = u & 0x7FFFFFu;
    if (exp == 0xFF && frac) return u2f(0x7FC00000u);
    if (exp == 0) return u2f(sign | 0x7F800000u);
    if (sign) return u2f(0x7FC00000u);
    if (exp == 0xFF) return 0.0f;
    uint32_t scaled = (exp & 1) == 0 ? (256 | (frac >> 15)) : (128 | (frac >> 16));
    uint32_t est = rsqrt_estimate_(scaled);
    uint32_t rexp = (380 - exp) / 2;
    return u2f(((rexp & 0xFF) << 23) | ((est & 0xFF) << 15));
}

#ifdef __cplusplus
}
#endif
