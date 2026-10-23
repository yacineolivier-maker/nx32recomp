/* Configuration utilisateur : <exécutable>.ini à côté de l'exécutable (créé avec les valeurs par défaut s'il manque).
 * Format INI simple : [section], cle = valeur, commentaires après « ; » ou « # ». */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "jeu.h"

namespace {

/* Version du format : un fichier plus ancien est réécrit au démarrage (copie dans <exécutable>.ini.ancien) avec les nouvelles
 * clés ; les valeurs de l'utilisateur sont conservées, sauf celles restées à un ancien défaut (ANCIENS_DEFAUTS). */
constexpr int VERSION_INI = 1; /* version du runtime ; version complète : VERSION_INI * 1000 + jeu_ini_version() */

const char *DEFAUT_RUNTIME = R"(; Configuration du portage PC.
; Fichier créé automatiquement : supprimez-le pour revenir aux valeurs par défaut.

[portage]
version_ini = 0            ; ne pas modifier (mise à jour automatique de ce fichier)

[fenetre]
largeur = 1280
hauteur = 720
plein_ecran = oui          ; oui / non (F11 bascule pendant le jeu)

[affichage]
resolution_interne = auto  ; multiplicateur du rendu du jeu (1080p sur station) : auto (selon l'écran : 1080p -> 1,
                           ; 1440p -> 1.5, 4K -> 2), 0.5, 0.75, 1, 1.5, 2, 3, 4...
filtre = bilineaire        ; mise à l'échelle vers la fenêtre : bilineaire, bicubique, fsr, area, lanczos, gaussien,
                           ; scaleforce, plus_proche
nettete_fsr = 25           ; 0 à 200 (filtre fsr)
antialiasing = smaa        ; aucun, fxaa, smaa
anisotropie = 16           ; auto, defaut, 2, 4, 8, 16 (netteté des textures vues en biais)
synchro_verticale = oui    ; oui, non, mailbox
format_image = 16_9        ; 16_9, 4_3, 21_9, 16_10, etire
hdr = non                  ; oui : sortie HDR10 (écran HDR, « Utiliser le HDR » activé dans Windows) ; non : SDR
hdr_blanc = 203            ; HDR : luminosité de l'image du jeu (blanc de référence), en nits
hdr_crete = 1000           ; HDR : luminosité maximale de l'écran, en nits (les reflets vifs montent jusque-là)
hdr_saturation = 100       ; HDR : saturation des couleurs, en % (100 = couleurs d'origine)

[manettes]
vibration = oui            ; oui / non
intensite_vibration = 100  ; en %
zone_morte = 10            ; zone morte des sticks, en % de la course
disposition = xbox         ; xbox : boutons selon leur lettre (A de la manette = A) ; nintendo : selon leur position
                           ; (bouton du bas = B, comme sur Switch)

[systeme]
langue = fr                ; ja, en-US, en-GB, fr, fr-CA, de, it, es, es-419, nl, pt, ru, ko, zh-CN, zh-TW
region = europe            ; japon, usa, europe, australie, chine, coree, taiwan
pseudo = Joueur            ; nom du joueur (10 caractères au plus)

[mods]
actifs = oui               ; fichiers de mods/<nom>/romfs/ (textures HD...) ; « -nom » désactive un mod
)";

int version_complete() { return VERSION_INI * 1000 + jeu_ini_version(); }

/* ini par défaut : runtime puis sections du jeu, avec la version complète */
const std::string &defaut() {
    static const std::string d = [] {
        std::string t = DEFAUT_RUNTIME;
        const std::string v = "version_ini = 0  ";
        t.replace(t.find(v), v.size(), "version_ini = " + std::to_string(version_complete()));
        const char *jeu = jeu_ini_defaut();
        if (jeu && *jeu) t += std::string("\n") + jeu;
        return t;
    }();
    return d;
}

/* anciens défauts (versions précédentes) remplacés par les nouveaux lors de la mise à jour du fichier */
const struct { const char *cle, *valeur; } ANCIENS_DEFAUTS[] = {
    {"affichage.resolution_interne", "1"}, {"affichage.antialiasing", "aucun"}, {"affichage.anisotropie", "auto"}};

std::mutex g_m;
std::map<std::string, std::string> g_valeurs; /* « section.cle » -> valeur */
bool g_charge = false;

std::string chemin_ini() {
    char chemin[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, chemin, MAX_PATH);
    std::string c(chemin, n); /* <exécutable>.ini */
    const size_t point = c.find_last_of('.');
    if (point != std::string::npos && point > c.find_last_of('\\')) c.resize(point);
    return c + ".ini";
}

std::string nettoyer(std::string s) {
    size_t c = s.find_first_of(";#");
    if (c != std::string::npos) s.resize(c);
    size_t d = s.find_first_not_of(" \t\r\n"), f = s.find_last_not_of(" \t\r\n");
    return d == std::string::npos ? "" : s.substr(d, f - d + 1);
}

/* lit un texte INI : « section.cle » -> valeur */
std::map<std::string, std::string> analyser(const std::string &texte) {
    std::map<std::string, std::string> v;
    std::string section;
    size_t p = 0;
    while (p < texte.size()) {
        size_t q = texte.find('\n', p);
        std::string l = nettoyer(texte.substr(p, q == std::string::npos ? std::string::npos : q - p));
        p = q == std::string::npos ? texte.size() : q + 1;
        if (l.empty()) continue;
        if (l.front() == '[' && l.back() == ']') { section = l.substr(1, l.size() - 2); continue; }
        size_t eg = l.find('=');
        if (eg != std::string::npos) v[section + "." + nettoyer(l.substr(0, eg))] = nettoyer(l.substr(eg + 1));
    }
    return v;
}

/* Fichier d'une version antérieure : défaut() où chaque valeur est remplacée par celle de l'utilisateur (sauf ancien
 * défaut), puis les clés inconnues de défaut() recopiées à la fin de leur section. */
std::string mettre_a_jour(const std::map<std::string, std::string> &utilisateur) {
    auto valeur_gardee = [&](const std::string &cle, std::string &v) {
        auto it = utilisateur.find(cle);
        if (it == utilisateur.end() || it->second.empty()) return false;
        for (auto &a : ANCIENS_DEFAUTS)
            if (cle == a.cle && it->second == a.valeur) return false;
        v = it->second;
        return true;
    };
    const std::map<std::string, std::string> defauts = analyser(defaut());
    std::string sortie, section, texte = defaut();
    auto completer_section = [&] { /* clés de l'utilisateur absentes de défaut(), dans la section qui se termine */
        if (section.empty()) return;
        const std::string prefixe = section + ".";
        for (auto &[k, v] : utilisateur)
            if (k.compare(0, prefixe.size(), prefixe) == 0 && !defauts.count(k)) sortie += k.substr(prefixe.size()) + " = " + v + "\n";
    };
    size_t p = 0;
    while (p < texte.size()) {
        size_t q = texte.find('\n', p);
        std::string brute = texte.substr(p, q == std::string::npos ? std::string::npos : q - p);
        p = q == std::string::npos ? texte.size() : q + 1;
        std::string l = nettoyer(brute);
        if (!l.empty() && l.front() == '[' && l.back() == ']') {
            const bool vide = sortie.size() >= 2 && sortie.compare(sortie.size() - 2, 2, "\n\n") == 0;
            if (vide) sortie.pop_back(); /* clés ajoutées avant la ligne vide qui précède la section suivante */
            completer_section();
            if (vide) sortie += "\n";
            section = l.substr(1, l.size() - 2);
        } else if (size_t eg = brute.find('='); !l.empty() && eg != std::string::npos && brute.find_first_of(";#") > eg) {
            std::string v, cle = nettoyer(brute.substr(0, eg));
            if (valeur_gardee(section + "." + cle, v) && cle != "version_ini") {
                size_t com = brute.find_first_of(";#");
                std::string debut = cle + " = " + v;
                if (com != std::string::npos) debut += std::string(debut.size() < com ? com - debut.size() : 1, ' ') + brute.substr(com);
                brute = debut;
            }
        }
        sortie += brute + "\n";
    }
    completer_section();
    return sortie;
}

std::string lire_fichier(const std::string &chemin) {
    std::string t;
    if (FILE *f = fopen(chemin.c_str(), "rb")) {
        char tampon[4096];
        size_t n;
        while ((n = fread(tampon, 1, sizeof tampon, f)) > 0) t.append(tampon, n);
        fclose(f);
    }
    return t;
}

bool ecrire_fichier(const std::string &chemin, const std::string &texte) {
    FILE *w = fopen(chemin.c_str(), "wb");
    if (!w) return false;
    fwrite(texte.data(), 1, texte.size(), w);
    fclose(w);
    return true;
}

void charger() {
    if (g_charge) return;
    g_charge = true;
    const std::string chemin = chemin_ini();
    std::string texte = lire_fichier(chemin);
    if (texte.empty()) { /* premier lancement : fichier par défaut, commenté */
        texte = defaut();
        if (ecrire_fichier(chemin, texte)) fprintf(stderr, "[config] %s créé (valeurs par défaut)\n", chemin.c_str());
    }
    g_valeurs = analyser(texte);
    auto v = g_valeurs.find("portage.version_ini");
    if ((v == g_valeurs.end() ? 1 : atoi(v->second.c_str())) < version_complete()) { /* fichier d'une version antérieure */
        const std::string neuf = mettre_a_jour(g_valeurs);
        if (ecrire_fichier(chemin + ".ancien", texte) && ecrire_fichier(chemin, neuf))
            fprintf(stderr, "[config] %s mis à jour (version %d ; ancien fichier : .ancien)\n", chemin.c_str(), version_complete());
        g_valeurs = analyser(neuf);
    }
}

} // namespace

/* valeur de [section] cle, ou defaut */
std::string config_texte(const char *section, const char *cle, const char *defaut) {
    std::lock_guard<std::mutex> lk(g_m);
    charger();
    auto it = g_valeurs.find(std::string(section) + "." + cle);
    return it == g_valeurs.end() || it->second.empty() ? defaut : it->second;
}

extern "C" int config_entier(const char *section, const char *cle, int defaut) {
    std::string v = config_texte(section, cle, "");
    return v.empty() ? defaut : atoi(v.c_str());
}

extern "C" int config_oui(const char *section, const char *cle, int defaut) {
    std::string v = config_texte(section, cle, "");
    if (v.empty()) return defaut;
    return v == "oui" || v == "1" || v == "vrai" || v == "true";
}

/* toutes les clés d'une section (ordre alphabétique) : rappel(cle, valeur) */
void config_section(const char *section, void (*rappel)(const char *cle, const char *valeur)) {
    std::map<std::string, std::string> copie;
    {
        std::lock_guard<std::mutex> lk(g_m);
        charger();
        copie = g_valeurs;
    }
    const std::string prefixe = std::string(section) + ".";
    for (auto &[k, v] : copie)
        if (k.compare(0, prefixe.size(), prefixe) == 0) rappel(k.c_str() + prefixe.size(), v.c_str());
}
