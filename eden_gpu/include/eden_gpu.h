/* API C de eden_gpu.dll : émulation GPU d'Eden (video_core + périphériques nvdrv) pour le pilote NVN d'origine du jeu.
 * Utilisée par le runtime natif en mode NX_PILOTE_ORIGINE. Mémoire invitée en identité (pointeur hôte = base + adresse
 * invitée 32 bits) ; événements noyau gérés par le runtime via des rappels. Code d'Eden sous GPL-3 (usage personnel). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef EDEN_GPU_EXPORTS
#define EDEN_API __declspec(dllexport)
#else
#define EDEN_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EdenRappels {
    void *ctx;
    /* crée un événement noyau : renvoie le handle d'écriture, *lisible = handle de lecture (pour le jeu) */
    uint32_t (*evenement_creer)(void *ctx, uint32_t *lisible);
    void (*evenement_signaler)(void *ctx, uint32_t ecriture); /* sûr entre threads */
    void (*evenement_effacer)(void *ctx, uint32_t ecriture);
    void (*journal)(void *ctx, const char *texte);
} EdenRappels;

/* option d'affichage (avant eden_gpu_init) : clés et valeurs de la section [affichage] de <exécutable>.ini
 * (resolution_interne, filtre, nettete_fsr, antialiasing, anisotropie, synchro_verticale, format_image) */
EDEN_API int eden_gpu_option(const char *cle, const char *valeur);
/* identifiant du titre (avant eden_gpu_init) : réglages par jeu et cache de shaders sur disque */
EDEN_API void eden_gpu_titre(uint64_t title_id);

/* hwnd : fenêtre Win32 de rendu (aucune autre swapchain dessus) ; memoire : base hôte de l'espace invité (4 Gio réservés
 * avec MEM_WRITE_WATCH) ; renvoie 0 si tout va bien */
EDEN_API int eden_gpu_init(void *hwnd, int largeur, int hauteur, uint8_t *memoire, const EdenRappels *rappels);
EDEN_API void eden_gpu_fenetre(int largeur, int hauteur);
/* écriture imminente de la mémoire invitée par le noyau (ReadFile dans un tampon du jeu) : à appeler avant, sinon une
 * page en cache du GPU (lecture seule) ferait échouer l'appel système */
EDEN_API void eden_gpu_ecriture_hote(uint64_t adresse, uint64_t taille);
/* 1 si la cohérence passe par GetWriteWatch (NX_EDEN_SUIVI=ww : mémoire réservée avec MEM_WRITE_WATCH) */
EDEN_API int eden_gpu_suivi_write_watch(void);
EDEN_API void eden_gpu_arreter(void);
/* capture BMP de la prochaine image composée (largeur x hauteur) ; 0 si une capture est déjà en cours */
EDEN_API int eden_gpu_capture(const char *chemin, int largeur, int hauteur);

/* nvdrv : mêmes conventions que le service (NvResult en retour) */
EDEN_API int32_t eden_nvdrv_open(const char *chemin);
EDEN_API uint32_t eden_nvdrv_ioctl(int32_t fd, uint32_t commande, const uint8_t *in, size_t n_in, uint8_t *out, size_t n_out,
                                   const uint8_t *in2, size_t n_in2, uint8_t *out2, size_t n_out2, int variante);
EDEN_API uint32_t eden_nvdrv_close(int32_t fd);
EDEN_API uint32_t eden_nvdrv_query_event(int32_t fd, uint32_t id, uint32_t *lisible);

/* vi : file de tampons Android (IGraphicBufferProducer) par couche, composée sur l'écran à chaque vsync */
EDEN_API int32_t eden_vi_creer_couche(void); /* -> id binder du producteur (parcel de OpenLayer), -1 si échec */
EDEN_API void eden_vi_detruire_couche(int32_t producteur);
/* parcels bruts (en-tête compris) ; renvoie 0 si le binder est inconnu */
EDEN_API int eden_vi_transact(int32_t binder, uint32_t code, const uint8_t *in, size_t n_in, uint8_t *out, size_t n_out,
                              uint32_t flags);
EDEN_API int eden_vi_evenement(int32_t binder, uint32_t type, uint32_t *lisible);
EDEN_API int eden_vi_composer(void); /* à chaque vsync ; renvoie l'intervalle de présentation (0 : rien à composer) */

/* Mii (code d'Eden) : CharInfo de 0x58 octets ; renvoient 1 si tout va bien */
EDEN_API int eden_mii_defaut(uint32_t index, uint8_t *sortie);             /* 6 Mii par défaut du firmware */
EDEN_API int eden_mii_base(uint32_t genre, uint8_t *sortie);
EDEN_API int eden_mii_aleatoire(uint32_t age, uint32_t genre, uint32_t race, uint8_t *sortie);
EDEN_API uint32_t eden_mii_verifier(const uint8_t *charinfo);              /* 0 : valide */

#ifdef __cplusplus
}
#endif
