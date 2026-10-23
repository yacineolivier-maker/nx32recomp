/* Mixeur audio natif (audren -> SDL3), voir mixeur.cpp */
#pragma once
#include <cstdint>

struct Mixeur;
Mixeur *mixeur_creer(uint32_t frequence, uint32_t echantillons, uint32_t nb_voix, uint32_t nb_mix, uint32_t nb_tampons_mix, uint32_t nb_sorties);
/* entree : adresse invitée de l'entrée de RequestUpdate ; sortie_voix : adresse des états de voix dans la sortie */
void mixeur_mise_a_jour(Mixeur *mx, uint32_t entree, uint32_t sortie_voix);
void mixeur_trame(Mixeur *mx); /* une trame (echantillons) mixée et envoyée à la sortie */
