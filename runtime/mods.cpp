/* Mods : RomFS virtuel = romfs.bin + fichiers des dossiers mods/<nom>/romfs/ (remplacements et ajouts), comme le
 * LayeredFS d'Atmosphère/Eden. Les métadonnées (tables de dossiers/fichiers + tables de hachage) sont reconstruites
 * entièrement ; les données d'origine restent lues dans romfs.bin, celles des mods dans leurs fichiers hôtes.
 * Disposition de l'image virtuelle :
 *   [0, 0x50)                en-tête reconstruit
 *   [0x50, taille d'origine) romfs.bin tel quel (ses anciennes métadonnées ne sont plus référencées)
 *   puis                     données des fichiers des mods (alignées sur 16)
 *   puis                     nouvelles métadonnées
 * Fichiers générés (ImageRomfs::generateurs) : contenu transformé en mémoire, placé après les données des mods.
 * Format : https://switchbrew.org/wiki/RomFS. Les mods de code (exefs, IPS) ne s'appliquent pas au code recompilé. */
#include "mods.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>

namespace fs = std::filesystem;

namespace {

constexpr uint32_t VIDE = 0xFFFFFFFF;

struct EnTete {
    uint64_t taille_entete, dir_hash_off, dir_hash_taille, dir_meta_off, dir_meta_taille;
    uint64_t fic_hash_off, fic_hash_taille, fic_meta_off, fic_meta_taille, donnees_off;
};
static_assert(sizeof(EnTete) == 0x50, "en-tête RomFS");

struct Fichier {
    uint64_t pos = 0, taille = 0; /* position dans l'image virtuelle */
    std::string hote;             /* vide : données dans romfs.bin */
};
struct Dossier {
    std::map<std::string, Dossier> dossiers; /* ordre trié : déterministe */
    std::map<std::string, Fichier> fichiers;
};

std::string u8(const fs::path &p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); }

uint32_t rd32(const std::vector<uint8_t> &v, size_t o) { uint32_t x; memcpy(&x, v.data() + o, 4); return x; }
uint64_t rd64(const std::vector<uint8_t> &v, size_t o) { uint64_t x; memcpy(&x, v.data() + o, 8); return x; }

/* arbre d'origine depuis les tables de romfs.bin */
void lire_dossier(const std::vector<uint8_t> &dirs, const std::vector<uint8_t> &fics, uint32_t off, uint64_t donnees,
                  Dossier &d) {
    for (uint32_t f = rd32(dirs, off + 12); f != VIDE; f = rd32(fics, f + 4)) {
        Fichier &x = d.fichiers[std::string((const char *)fics.data() + f + 0x20, rd32(fics, f + 0x1C))];
        x.pos = donnees + rd64(fics, f + 8);
        x.taille = rd64(fics, f + 16);
    }
    for (uint32_t c = rd32(dirs, off + 8); c != VIDE; c = rd32(dirs, c + 4))
        lire_dossier(dirs, fics, c, donnees,
                     d.dossiers[std::string((const char *)dirs.data() + c + 0x18, rd32(dirs, c + 0x14))]);
}

/* fichiers d'un dossier de mod par-dessus l'arbre */
void fusionner(const fs::path &racine, Dossier &d, std::vector<std::pair<Fichier *, uint64_t>> &ajouts, int &n) {
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(racine, ec)) {
        const std::string nom = u8(e.path().filename());
        if (e.is_directory(ec)) fusionner(e.path(), d.dossiers[nom], ajouts, n);
        else if (e.is_regular_file(ec)) {
            Fichier &f = d.fichiers[nom];
            f.hote = u8(e.path());
            ajouts.emplace_back(&f, e.file_size(ec));
            n++;
        }
    }
}

uint32_t hachage(uint32_t parent, const std::string &nom) {
    uint32_t h = parent ^ 123456789;
    for (unsigned char c : nom) h = ((h >> 5) | (h << 27)) ^ c;
    return h;
}
uint32_t nb_seaux(uint32_t n) { /* comme Atmosphère (fsmitm_romfs) */
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    while (n % 2 == 0 || n % 3 == 0 || n % 5 == 0 || n % 7 == 0 || n % 11 == 0 || n % 13 == 0 || n % 17 == 0) n++;
    return n;
}

/* sérialisation : décalages attribués en parcours en largeur (racine à 0) */
struct Constructeur {
    struct Entree { const std::string *nom; uint32_t parent, off; };
    std::vector<uint8_t> dirs, fics;
    std::vector<Entree> e_dirs, e_fics;
    uint64_t donnees_off = 0;

    static uint32_t taille_entree(size_t base, size_t nom) { return (uint32_t)(base + ((nom + 3) & ~size_t(3))); }

    void construire(const Dossier &racine) {
        static const std::string vide;
        /* 1) décalages */
        std::vector<std::pair<const Dossier *, uint32_t>> file{{&racine, 0}};
        std::map<const Dossier *, uint32_t> off_dir{{&racine, 0}};
        e_dirs.push_back({&vide, 0, 0});
        uint32_t td = taille_entree(0x18, 0), tf = 0;
        for (size_t i = 0; i < file.size(); i++) {
            const Dossier *d = file[i].first;
            for (const auto &[nom, sd] : d->dossiers) {
                off_dir[&sd] = td;
                e_dirs.push_back({&nom, off_dir[d], td});
                td += taille_entree(0x18, nom.size());
                file.push_back({&sd, 0});
            }
        }
        std::map<const Fichier *, uint32_t> off_fic;
        for (const auto &[d, _] : file)
            for (const auto &[nom, f] : d->fichiers) {
                off_fic[&f] = tf;
                e_fics.push_back({&nom, off_dir[d], tf});
                tf += taille_entree(0x20, nom.size());
            }
        dirs.assign(td, 0);
        fics.assign(tf, 0);
        auto w32 = [](std::vector<uint8_t> &v, size_t o, uint32_t x) { memcpy(v.data() + o, &x, 4); };
        auto w64 = [](std::vector<uint8_t> &v, size_t o, uint64_t x) { memcpy(v.data() + o, &x, 8); };
        /* 2) entrées */
        for (const auto &[d, _] : file) {
            const uint32_t o = off_dir[d];
            w32(dirs, o + 8, d->dossiers.empty() ? VIDE : off_dir[&d->dossiers.begin()->second]);
            w32(dirs, o + 12, d->fichiers.empty() ? VIDE : off_fic[&d->fichiers.begin()->second]);
            for (auto it = d->dossiers.begin(); it != d->dossiers.end(); ++it) {
                auto suiv = std::next(it);
                w32(dirs, off_dir[&it->second] + 4, suiv == d->dossiers.end() ? VIDE : off_dir[&suiv->second]);
            }
            for (auto it = d->fichiers.begin(); it != d->fichiers.end(); ++it) {
                auto suiv = std::next(it);
                const uint32_t fo = off_fic[&it->second];
                w32(fics, fo + 4, suiv == d->fichiers.end() ? VIDE : off_fic[&suiv->second]);
                w64(fics, fo + 8, it->second.pos - donnees_off);
                w64(fics, fo + 16, it->second.taille);
            }
        }
        for (const auto &e : e_dirs) {
            w32(dirs, e.off + 0, e.parent);
            w32(dirs, e.off + 0x14, (uint32_t)e.nom->size());
            memcpy(dirs.data() + e.off + 0x18, e.nom->data(), e.nom->size());
        }
        for (const auto &e : e_fics) {
            w32(fics, e.off + 0, e.parent);
            w32(fics, e.off + 0x1C, (uint32_t)e.nom->size());
            memcpy(fics.data() + e.off + 0x20, e.nom->data(), e.nom->size());
        }
    }
    /* table de hachage + chaînage hash_next (à +0x10 pour les dossiers, +0x18 pour les fichiers) */
    static std::vector<uint32_t> table(std::vector<uint8_t> &v, const std::vector<Entree> &es, size_t champ) {
        std::vector<uint32_t> t(nb_seaux((uint32_t)es.size()), VIDE);
        for (const auto &e : es) {
            uint32_t &s = t[hachage(e.parent, *e.nom) % t.size()];
            memcpy(v.data() + e.off + champ, &s, 4);
            s = e.off;
        }
        return t;
    }
};

} // namespace

bool ImageRomfs::ouvrir(const std::string &romfs, const std::string &dossier_mods) {
    FILE *f = fopen(romfs.c_str(), "rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    const uint64_t taille_origine = (uint64_t)_ftelli64(f);
    EnTete h{};
    _fseeki64(f, 0, SEEK_SET);
    if (fread(&h, sizeof h, 1, f) != 1 || h.taille_entete != 0x50) { fclose(f); return false; }
    std::vector<uint8_t> dirs(h.dir_meta_taille), fics(h.fic_meta_taille);
    _fseeki64(f, (long long)h.dir_meta_off, SEEK_SET);
    fread(dirs.data(), 1, dirs.size(), f);
    _fseeki64(f, (long long)h.fic_meta_off, SEEK_SET);
    fread(fics.data(), 1, fics.size(), f);
    fclose(f);

    Dossier racine;
    lire_dossier(dirs, fics, 0, h.donnees_off, racine);

    /* mods : chaque mods/<nom>/romfs (ou mods/<nom> directement), par ordre alphabétique (le dernier l'emporte) ;
     * un dossier dont le nom commence par « - » est ignoré (mod désactivé) */
    std::vector<fs::path> sources;
    std::error_code ec;
    if (!dossier_mods.empty())
        for (const auto &e : fs::directory_iterator(fs::u8path(dossier_mods), ec)) {
            if (!e.is_directory(ec) || u8(e.path().filename()).rfind("-", 0) == 0) continue;
            sources.push_back(fs::exists(e.path() / "romfs", ec) ? e.path() / "romfs" : e.path());
        }
    std::sort(sources.begin(), sources.end());
    std::vector<std::pair<Fichier *, uint64_t>> ajouts;
    for (const auto &s : sources) {
        int n = 0;
        fusionner(s, racine, ajouts, n);
        fprintf(stderr, "[mods] %s : %d fichier(s)\n", u8(s).c_str(), n);
    }
    std::map<Fichier *, uint64_t> uniques;
    for (const auto &[fic, t] : ajouts) uniques[fic] = t; /* taille du dernier mod */

    /* fichiers générés : contenu courant (mod ou romfs.bin) transformé en mémoire */
    generes_donnees.clear();
    std::map<Fichier *, size_t> generes; /* fichier -> indice dans generes_donnees */
    for (const auto &[chemin, generer] : generateurs) {
        Dossier *d = &racine;
        Fichier *fic = nullptr;
        for (size_t p = 1; p <= chemin.size() && d;) {
            const size_t q = std::min(chemin.find('/', p), chemin.size());
            const std::string nom = chemin.substr(p, q - p);
            if (q == chemin.size()) {
                auto it = d->fichiers.find(nom);
                fic = it == d->fichiers.end() ? nullptr : &it->second;
            } else {
                auto it = d->dossiers.find(nom);
                d = it == d->dossiers.end() ? nullptr : &it->second;
            }
            p = q + 1;
        }
        if (!fic) { fprintf(stderr, "[mods] fichier à générer absent : %s\n", chemin.c_str()); continue; }
        const bool de_mod = !fic->hote.empty();
        std::vector<uint8_t> origine(de_mod ? uniques[fic] : fic->taille), sortie;
        if (FILE *g = de_mod ? _wfopen(fs::u8path(fic->hote).c_str(), L"rb") : fopen(romfs.c_str(), "rb")) {
            _fseeki64(g, de_mod ? 0 : (long long)fic->pos, SEEK_SET);
            const size_t lu = fread(origine.data(), 1, origine.size(), g);
            fclose(g);
            if (lu != origine.size()) continue;
        } else continue;
        if (!generer(origine, sortie)) continue;
        generes[fic] = generes_donnees.size();
        generes_donnees.push_back(std::move(sortie));
        uniques.erase(fic);
        fprintf(stderr, "[mods] %s généré (%zu -> %zu octets)\n", chemin.c_str(), origine.size(), generes_donnees.back().size());
    }
    if (uniques.empty() && generes.empty()) return false;

    /* données des mods après romfs.bin (un fichier remplacé deux fois n'est placé qu'une fois : le dernier), puis
     * celles des fichiers générés */
    segments.clear();
    entete.assign(0x50, 0);
    segments.push_back({0, 0x50, Segment::ENTETE, {}, 0});
    segments.push_back({0x50, taille_origine - 0x50, Segment::ORIGINE, romfs, 0x50});
    uint64_t pos = (taille_origine + 15) & ~uint64_t(15);
    for (auto &[fic, t] : uniques) {
        fic->pos = pos;
        fic->taille = t;
        if (t) segments.push_back({pos, t, Segment::HOTE, fic->hote, 0});
        pos = (pos + t + 15) & ~uint64_t(15);
    }
    for (auto &[fic, i] : generes) {
        const uint64_t t = generes_donnees[i].size();
        fic->pos = pos;
        fic->taille = t;
        if (t) segments.push_back({pos, t, Segment::MEMOIRE, {}, i});
        pos = (pos + t + 15) & ~uint64_t(15);
    }

    Constructeur c;
    c.donnees_off = h.donnees_off;
    c.construire(racine);
    auto th_d = Constructeur::table(c.dirs, c.e_dirs, 0x10);
    auto th_f = Constructeur::table(c.fics, c.e_fics, 0x18);

    EnTete n = h;
    auto ajouter = [&](const void *p, size_t t, uint64_t &off, uint64_t &taille) {
        off = pos;
        taille = t;
        meta.insert(meta.end(), (const uint8_t *)p, (const uint8_t *)p + t);
        pos += t;
    };
    const uint64_t debut_meta = pos;
    ajouter(th_d.data(), th_d.size() * 4, n.dir_hash_off, n.dir_hash_taille);
    ajouter(c.dirs.data(), c.dirs.size(), n.dir_meta_off, n.dir_meta_taille);
    ajouter(th_f.data(), th_f.size() * 4, n.fic_hash_off, n.fic_hash_taille);
    ajouter(c.fics.data(), c.fics.size(), n.fic_meta_off, n.fic_meta_taille);
    segments.push_back({debut_meta, meta.size(), Segment::META, {}, 0});
    memcpy(entete.data(), &n, sizeof n);
    taille = pos;
    fprintf(stderr, "[mods] RomFS virtuel : %zu fichier(s) de mods, %zu généré(s), %zu dossiers, %zu fichiers au total\n",
            uniques.size(), generes.size(), c.e_dirs.size(), c.e_fics.size());
    return true;
}

void ImageRomfs::lire(uint64_t off, uint64_t n, uint8_t *dst) {
    std::lock_guard<std::mutex> lk(m);
    memset(dst, 0, (size_t)n); /* trous d'alignement */
    for (const auto &s : segments) {
        if (off + n <= s.debut || off >= s.debut + s.taille) continue;
        const uint64_t a = std::max(off, s.debut), b = std::min(off + n, s.debut + s.taille);
        uint8_t *d = dst + (a - off);
        switch (s.type) {
        case Segment::ENTETE: memcpy(d, entete.data() + (a - s.debut), (size_t)(b - a)); break;
        case Segment::META: memcpy(d, meta.data() + (a - s.debut), (size_t)(b - a)); break;
        case Segment::MEMOIRE: memcpy(d, generes_donnees[s.decalage].data() + (a - s.debut), (size_t)(b - a)); break;
        default: {
            FILE *&f = ouverts[s.hote];
            if (!f) f = _wfopen(fs::u8path(s.hote).c_str(), L"rb");
            if (!f) { fprintf(stderr, "[mods] lecture impossible : %s\n", s.hote.c_str()); break; }
            _fseeki64(f, (long long)(s.decalage + (a - s.debut)), SEEK_SET);
            fread(d, 1, (size_t)(b - a), f);
            if (ouverts.size() > 64) { /* limite de descripteurs ouverts */
                for (auto &[k, g] : ouverts) if (g && g != f) fclose(g);
                FILE *garde = f;
                std::string cle = s.hote;
                ouverts.clear();
                ouverts[cle] = garde;
            }
        }
        }
    }
}

ImageRomfs::~ImageRomfs() {
    for (auto &[k, f] : ouverts) if (f) fclose(f);
}
