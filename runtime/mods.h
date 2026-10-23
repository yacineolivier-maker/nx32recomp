/* Mods : RomFS virtuel (romfs.bin + mods/<nom>/romfs/ + fichiers générés en mémoire), voir mods.cpp */
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct ImageRomfs {
    struct Segment {
        uint64_t debut, taille;
        enum Type { ENTETE, ORIGINE, HOTE, META, MEMOIRE } type;
        std::string hote;
        uint64_t decalage; /* HOTE/ORIGINE : position dans le fichier ; MEMOIRE : indice dans generes_donnees */
    };
    /* Fichier généré : transforme le contenu d'un fichier du RomFS (celui d'un mod s'il est remplacé) ; renvoie faux
     * pour le laisser tel quel. Clé : chemin absolu dans le RomFS (« /Common/Gfx/x.sarc »). */
    using Generateur = std::function<bool(const std::vector<uint8_t> &origine, std::vector<uint8_t> &sortie)>;
    std::map<std::string, Generateur> generateurs;

    std::vector<Segment> segments;
    std::vector<uint8_t> entete, meta;
    std::vector<std::vector<uint8_t>> generes_donnees;
    uint64_t taille = 0;
    std::map<std::string, FILE *> ouverts;
    std::mutex m;

    /* faux si rien ne change (ni mod ni fichier généré : lire alors romfs.bin directement) ; dossier_mods vide :
     * mods ignorés */
    bool ouvrir(const std::string &romfs, const std::string &dossier_mods);
    void lire(uint64_t off, uint64_t n, uint8_t *dst);
    ~ImageRomfs();
};
