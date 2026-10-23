/* Remplacements natifs de fonctions du SDK (liste : <profil>/hle.txt, cf. profil/exemple/hle.txt).
 * Les remplacements propres à un jeu (par adresse) sont enregistrés par jeu_hle_init (rt_hle_register). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "funcs.h"
#include "jeu.h"

/* ------------------------------------------------------------------ graphismes (subsdk nvn) */
/* Le pilote NVN du jeu (recompilé) est utilisé tel quel ; ses commandes GPU passent par les services nvdrv vers
 * l'émulation GPU d'Eden (eden_gpu.dll). nvnBootstrapLoader n'est remplacé que pour la trace (NX_NVN_TRACE_ORIGINE). */
uint32_t nvn_bootstrap_origine(uint32_t nom, uint32_t orig);
#ifdef ORIG_nvnBootstrapLoader
void HLE_nvnBootstrapLoader(Cpu *c) {
    uint32_t nom = c->r[0];
    ORIG_nvnBootstrapLoader(c);
    c->r[0] = nvn_bootstrap_origine(nom, c->r[0]);
    fprintf(stderr, "[nvn-origine] nvnBootstrapLoader(\"%s\") -> %08X\n", (const char *)(g_mem + nom), c->r[0]);
}
#endif

#ifdef ORIG__ZN2nv22DumpGraphicsMemoryInfoEv
/* nv::DumpGraphicsMemoryInfo() : diagnostic du pilote */
void HLE__ZN2nv22DumpGraphicsMemoryInfoEv(Cpu *c) { fprintf(stderr, "[hle] nv::DumpGraphicsMemoryInfo ignoré (lr=%08X)\n", c->r[14]); }
#endif

#ifdef ORIG_NvOsDrvOpen
/* NvOsDrvOpen(chemin) : périphériques GPU ouverts via le service nvdrv ; les décodeurs matériels (nvdec, vic, nvjpg)
 * sont refusés, la vidéo passant par le décodeur natif (movie.cpp) */
void HLE_NvOsDrvOpen(Cpu *c) {
    const char *chemin = (const char *)(g_mem + c->r[0]);
    if (!strstr(chemin, "nvdec") && !strstr(chemin, "vic") && !strstr(chemin, "nvjpg")) {
        ORIG_NvOsDrvOpen(c);
        return;
    }
    fprintf(stderr, "[hle] NvOsDrvOpen(\"%s\") refusé (lr=%08X)\n", chemin, c->r[14]);
    c->r[0] = (uint32_t)-1;
}
#endif

#ifdef ORIG__ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatE
/* movie::Decoder::Decoder(type, format de sortie) : décodeur natif (runtime/movie.cpp) */
void movie_decoder_construire(Cpu *c);
void HLE__ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatE(Cpu *c) {
    if (jeu_adresses_movie())
        movie_decoder_construire(c);
    else
        ORIG__ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatE(c);
}
#endif
