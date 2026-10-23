/* Jeu d'exemple : valeurs neutres de l'interface runtime/jeu.h (aucun correctif, aucune section d'ini). */
#include "jeu.h"

extern "C" const char *jeu_nom(void) { return "Portage PC"; }
extern "C" uint64_t jeu_title_id(void) { return 0x0100000000010000ull; }
extern "C" void jeu_correctifs_image(uint32_t) {}
extern "C" void jeu_hle_init(void) {}
extern "C" void jeu_bilan(void) {}
extern "C" const char *jeu_ini_defaut(void) { return ""; }
extern "C" int jeu_ini_version(void) { return 0; }
extern "C" int jeu_largeur_station(void) { return 1920; }
extern "C" const char *jeu_format_image(void) { return nullptr; }
extern "C" const JeuAdressesMovie *jeu_adresses_movie(void) { return nullptr; }
void jeu_generateurs_romfs(ImageRomfs &) {}
