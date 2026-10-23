/* Interface d'un jeu : tout ce qui est propre au titre recompilé est fourni par une bibliothèque du projet du jeu
 * (cible CMake « jeu », dossier NX_JEU_DIR), hors de ce dépôt. Exemple minimal : jeux/exemple/jeu.cpp. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* nom affiché (titre de la fenêtre, nom de l'application Vulkan) */
const char *jeu_nom(void);

/* identifiant du titre (ProgramId), ex. 0x0100000000010000 */
uint64_t jeu_title_id(void);

/* après le chargement de l'image, avant tout code du jeu : correctifs de données (réglages de l'ini) ;
 * base_main : adresse de chargement du module main */
void jeu_correctifs_image(uint32_t base_main);

/* au démarrage du thread principal, avant l'initialisation de la libc : remplacements natifs enregistrés par adresse
 * (rt_hle_register), sondes de diagnostic */
void jeu_hle_init(void);

/* bilan périodique de diagnostic (chien de garde NX_DUMP_THREADS) ; peut ne rien faire */
void jeu_bilan(void);

/* sections propres au jeu ajoutées à l'ini par défaut (texte INI commenté, "" si aucune) et leur version : un
 * fichier d'une version antérieure est mis à jour en gardant les valeurs de l'utilisateur */
const char *jeu_ini_defaut(void);
int jeu_ini_version(void);

/* largeur de l'écran annoncée au jeu en station (1920 d'origine ; plus si le jeu est rendu en ultra-large) */
int jeu_largeur_station(void);

/* format d'image imposé par le jeu (ex. « etire » si l'image a déjà le format de l'écran), NULL : réglage de
 * l'utilisateur */
const char *jeu_format_image(void);

/* adresses invitées utilisées par le décodeur vidéo natif (runtime/movie.cpp) : fonctions du SDK et de la bibliothèque
 * multimédia, clés de movie::MediaData (chaînes de l'image du jeu) */
typedef struct JeuAdressesMovie {
    uint32_t signal_event, operator_delete; /* nn::os::SignalEvent, opérateur delete de la bibliothèque */
    uint32_t md_set_int32, md_find_int32, md_set_string, md_find_buffer, md_set_rect; /* movie::MediaData */
    uint32_t s_mime, s_width, s_height, s_stride, s_crop, s_avcc, s_esds, s_sample_rate, s_channel_count;
    uint32_t s_video_avc, s_audio_aac;
} JeuAdressesMovie;
/* NULL : movie::Decoder d'origine (décodeur matériel, non émulé) */
const JeuAdressesMovie *jeu_adresses_movie(void);

#ifdef __cplusplus
}

struct ImageRomfs;
/* fichiers du RomFS réécrits à la volée (mods.h : ImageRomfs::generateurs) ; peut ne rien faire */
void jeu_generateurs_romfs(ImageRomfs &img);
#endif
