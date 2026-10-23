/* Lien avec eden_gpu.dll (émulation GPU d'Eden, mode par défaut « pilote d'origine ») : initialisation sur la fenêtre du portage,
 * événements noyau d'Eden reliés aux événements du runtime (svc), périphériques nvdrv relayés vers la DLL.
 * La DLL est chargée en différé (/DELAYLOAD) : le mode normal (couche NVN maison) n'en dépend pas. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "eden_gpu.h"
#include "nvdrv.h"

extern "C" {
#include "cpu.h"
void svc_attente_debut(void);
void svc_attente_fin(void);
}

namespace {

bool g_actif = false;

uint32_t ev_creer(void *, uint32_t *lisible) {
    uint32_t r = 0, w = svc_new_event(0, &r);
    *lisible = r;
    return w;
}
void ev_signaler(void *, uint32_t w) { svc_signal_event(w); }
void ev_effacer(void *, uint32_t w) { svc_clear_event(w); }
/* WriteFile direct (pas de verrou du CRT : utilisable après la suspension des threads du jeu à l'arrêt) */
void ecrire_err(const char *texte) {
    DWORD n;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), texte, (DWORD)strlen(texte), &n, nullptr);
}
void journal(void *, const char *texte) {
    char ligne[1200];
    snprintf(ligne, sizeof ligne, "[eden] %s\n", texte);
    ecrire_err(ligne);
}

/* Périphérique /dev/nv* tenu par la DLL */
struct DevEden : NvDev {
    int32_t fd;
    std::string chemin;
    DevEden(int32_t f, std::string c) : fd(f), chemin(std::move(c)) {}
    ~DevEden() override { eden_nvdrv_close(fd); }
    uint32_t ioctl(NvIoctl cmd, const std::vector<uint8_t> &in, std::vector<uint8_t> &out, const std::vector<uint8_t> &in2,
                   std::vector<uint8_t> &out2) override {
        int variante = !in2.empty() ? 2 : !out2.empty() ? 3 : 1;
        /* appelé depuis une requête IPC : rt_svc a déjà rendu le cœur du thread invité pendant le service */
        return eden_nvdrv_ioctl(fd, cmd.raw, in.data(), in.size(), out.data(), out.size(), in2.data(), in2.size(), out2.data(),
                                out2.size(), variante);
    }
    uint32_t evenement(uint32_t id, uint32_t *h) override { return eden_nvdrv_query_event(fd, id, h); }
    int32_t fd_impose() const override { return fd; }
};

} // namespace

extern "C" int eden_actif(void) { return g_actif; }

/* écriture du noyau dans la mémoire invitée (fs : fread) : pages en cache du GPU rendues inscriptibles d'abord */
extern "C" void rt_ecriture_hote(uint32_t adresse, uint32_t taille) {
    if (g_actif) eden_gpu_ecriture_hote(adresse, taille);
}

/* Appelée à la création de la fenêtre (thread principal) */
void config_section(const char *section, void (*rappel)(const char *cle, const char *valeur)); /* config.cpp */

#include "jeu.h" /* jeu_format_image : image déjà au format de l'écran (ultra-large) => plein écran */
static int g_hauteur_ecran = 1080;

/* hauteur_ecran : définition verticale de l'écran (résolution interne « auto ») */
extern "C" int eden_demarrer(void *hwnd, int largeur, int hauteur, int hauteur_ecran) {
    g_hauteur_ecran = hauteur_ecran > 0 ? hauteur_ecran : hauteur;
    config_section("affichage", [](const char *cle, const char *valeur) {
        char choisie[16];
        if (!strcmp(cle, "resolution_interne") && !strcmp(valeur, "auto")) {
            /* plus petit multiplicateur dont le rendu (1080p x m) couvre l'écran : 1080p -> 1, 1440p -> 1.5, 4K -> 2 */
            const double voulu = g_hauteur_ecran / 1080.0 - 0.02;
            const char *m = "4";
            for (const char *c : {"1", "1.5", "2", "3"})
                if (atof(c) >= voulu) { m = c; break; }
            snprintf(choisie, sizeof choisie, "%s", m);
            fprintf(stderr, "[config] affichage : resolution_interne = auto -> %s (écran de %d lignes)\n", m, g_hauteur_ecran);
            valeur = choisie;
        }
        if (!strcmp(cle, "format_image") && jeu_format_image()) valeur = jeu_format_image();
        eden_gpu_option(cle, valeur);
        fprintf(stderr, "[config] affichage : %s = %s\n", cle, valeur);
    });
    eden_gpu_titre(jeu_title_id());
    EdenRappels r{};
    r.evenement_creer = ev_creer;
    r.evenement_signaler = ev_signaler;
    r.evenement_effacer = ev_effacer;
    r.journal = journal;
    if (eden_gpu_init(hwnd, largeur, hauteur, g_mem, &r) != 0) {
        fprintf(stderr, "[eden] initialisation du GPU émulé impossible\n");
        return 0;
    }
    g_actif = true;
    return 1;
}

/* Fermeture de la fenêtre : threads du jeu suspendus (plus d'appels nvdrv), puis arrêt du GPU émulé borné à 3 s
 * (un thread du jeu suspendu peut détenir un verrou d'Eden) ; l'appelant termine ensuite le processus sans
 * destructeurs statiques (TerminateProcess) */
extern "C" void svc_suspendre_invites(void);
extern "C" void eden_arret_propre(void) {
    if (!g_actif) return;
    fflush(stdout); /* avant la suspension : un thread suspendu peut détenir le verrou d'un FILE du CRT */
    fflush(stderr);
    svc_suspendre_invites();
    g_actif = false;
    HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD { eden_gpu_arreter(); return 0; }, nullptr, 0, nullptr);
    if (t && WaitForSingleObject(t, 3000) == WAIT_TIMEOUT) ecrire_err("[eden] arrêt du GPU émulé : délai dépassé\n");
}

extern "C" void eden_fenetre(int largeur, int hauteur) {
    if (g_actif) eden_gpu_fenetre(largeur, hauteur);
}

/* nvdrv Open en mode Eden : périphérique relayé vers la DLL (nullptr si le chemin y est inconnu) */
std::shared_ptr<NvDev> eden_ouvrir(const std::string &chemin) {
    int32_t fd = eden_nvdrv_open(chemin.c_str());
    if (fd < 0) return nullptr;
    return std::make_shared<DevEden>(fd, chemin);
}
