/* Décodeurs hôtes Media Foundation (Windows) pour les vidéos du jeu. */
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

struct IMFTransform;

struct ImageNV12 {
    std::vector<uint8_t> donnees; /* plan Y (pas x hauteur) puis plan UV entrelacé (pas x hauteur/2) */
    int largeur = 0, hauteur = 0, pas = 0, visible_l = 0, visible_h = 0;
    int64_t pts_us = 0;
};

struct DecodeurVideoMF {
    IMFTransform *t = nullptr;
    int largeur = 0, hauteur = 0, pas = 0, visible_l = 0, visible_h = 0;
    std::vector<ImageNV12> attente;
    bool ouvrir();
    void envoyer(const uint8_t *d, size_t n, int64_t pts_us); /* H.264 Annex-B (codes de début) */
    bool recevoir(ImageNV12 &img);
    void fin_de_flux();
    void vider();
    ~DecodeurVideoMF();

private:
    bool choisir_sortie();
};

struct BlocPCM {
    std::vector<uint8_t> donnees; /* PCM 16 bits entrelacé */
    int64_t pts_us = 0;
};

struct DecodeurAudioMF {
    IMFTransform *t = nullptr;
    int frequence = 0, canaux = 0;
    std::vector<BlocPCM> attente;
    bool ouvrir(const uint8_t *asc, size_t n_asc, int frequence, int canaux); /* asc : AudioSpecificConfig (csd-0) */
    void envoyer(const uint8_t *d, size_t n, int64_t pts_us);               /* trame AAC brute */
    bool recevoir(BlocPCM &b);
    void vider();
    ~DecodeurAudioMF();
};
