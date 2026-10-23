/* Manettes : SDL3 (manettes Xbox et autres) -> mémoire partagée HID (format nn::hid::detail::NpadInternalState).
 * Joueur i = manette d'indice de joueur SDL i (numéro XInput, LED), sinon premier emplacement libre ; style
 * « Pro Controller » (FullKey). Clavier en secours pour le joueur 1.
 * NX_SCRIPT="10:A,12.5:A,i1200:BAS,i1550:A@2" : appuis simulés pour les tests (secondes depuis le lancement, 100 ms ;
 * « iN » : n° d'image présentée, 6 images ; « @J » : joueur J, 1 par défaut — un joueur scripté sans manette est
 * connecté virtuellement). */
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>
#include <vector>

extern "C" {
#include "cpu.h"
}
#include "ipc.h"

extern uint32_t g_hid_shmem;
extern "C" volatile long rt_nb_presentations;

namespace {

/* nn::hid::NpadButton */
enum : uint64_t {
    B_A = 1ull << 0, B_B = 1ull << 1, B_X = 1ull << 2, B_Y = 1ull << 3, B_STICK_L = 1ull << 4, B_STICK_R = 1ull << 5,
    B_L = 1ull << 6, B_R = 1ull << 7, B_ZL = 1ull << 8, B_ZR = 1ull << 9, B_PLUS = 1ull << 10, B_MINUS = 1ull << 11,
    B_GAUCHE = 1ull << 12, B_HAUT = 1ull << 13, B_DROITE = 1ull << 14, B_BAS = 1ull << 15,
    B_LS_GAUCHE = 1ull << 16, B_LS_HAUT = 1ull << 17, B_LS_DROITE = 1ull << 18, B_LS_BAS = 1ull << 19,
    B_RS_GAUCHE = 1ull << 20, B_RS_HAUT = 1ull << 21, B_RS_DROITE = 1ull << 22, B_RS_BAS = 1ull << 23,
};

constexpr uint32_t NPAD_BASE = 0x9A00, NPAD_TAILLE = 0x5000, LIFO_FULLKEY = 0x28, LIFO_HANDHELD = 0x378;
constexpr uint32_t N_ENTREES = 17, TAILLE_ENTREE = 0x30;

struct Etat {
    uint64_t boutons = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    bool connecte = false;
};

SDL_Gamepad *g_manettes[8] = {};

/* Vibration (hid SendVibrationValue[s], thread IPC) : joueur -> manette SDL, amplitudes par moteur du Joy-Con
 * gauche/droit (basse 160 Hz / haute 320 Hz) ; g_m_vib protège aussi la fermeture des manettes. */
std::mutex g_m_vib;
SDL_JoystickID g_joueur_id[8] = {};
float g_vib[8][2][2] = {};
Uint16 g_vib_envoyee[8][2] = {};
Uint64 g_vib_t[8] = {};
int64_t g_echantillon = 0;
Uint64 g_t0 = 0;

struct AppuiScripte { double t; uint64_t bouton; bool image = false; int joueur = 0; }; /* image : t = n° de présentation */
int g_joueurs_scriptes = 1; /* nombre de joueurs cités par le script (connectés virtuellement s'il manque des manettes) */
std::vector<AppuiScripte> g_script;

uint64_t bouton_par_nom(const std::string &n) {
    static const struct { const char *nom; uint64_t b; } t[] = {
        {"A", B_A}, {"B", B_B}, {"X", B_X}, {"Y", B_Y}, {"L", B_L}, {"R", B_R}, {"ZL", B_ZL}, {"ZR", B_ZR}, {"PLUS", B_PLUS},
        {"MOINS", B_MINUS}, {"GAUCHE", B_GAUCHE}, {"HAUT", B_HAUT}, {"DROITE", B_DROITE}, {"BAS", B_BAS}};
    for (auto &x : t)
        if (n == x.nom) return x.b;
    return 0;
}

void lire_script() {
    const char *s = getenv("NX_SCRIPT");
    if (!s) return;
    std::string tout = s;
    size_t p = 0;
    while (p < tout.size()) {
        size_t v = tout.find(',', p);
        std::string e = tout.substr(p, v == std::string::npos ? std::string::npos : v - p);
        size_t d = e.find(':');
        if (d != std::string::npos) {
            bool img = e[0] == 'i';
            uint64_t b = 0; /* boutons combinés : « L+R » */
            std::string noms = e.substr(d + 1);
            int joueur = 0;
            if (size_t at = noms.find('@'); at != std::string::npos) {
                joueur = std::max(0, std::min(7, atoi(noms.c_str() + at + 1) - 1));
                noms.resize(at);
                g_joueurs_scriptes = std::max(g_joueurs_scriptes, joueur + 1);
            }
            for (size_t q = 0; q <= noms.size();) {
                size_t r = noms.find('+', q);
                b |= bouton_par_nom(noms.substr(q, r == std::string::npos ? std::string::npos : r - q));
                if (r == std::string::npos) break;
                q = r + 1;
            }
            g_script.push_back({atof(e.substr(img ? 1 : 0, d - (img ? 1 : 0)).c_str()), b, img, joueur});
        }
        if (v == std::string::npos) break;
        p = v + 1;
    }
    fprintf(stderr, "[manettes] script : %zu appuis\n", g_script.size());
}

/* réglages [manettes] de <exécutable>.ini */
double g_zone_morte = 0.10;           /* fraction de la course des sticks */
bool g_disposition_nintendo = false;  /* boutons selon leur position (bas = B) au lieu de leur lettre */
bool g_vibration = true;
double g_intensite_vibration = 1.0;

/* Stick en zone morte radiale : sous le seuil, centre ; au-delà, amplitude ramenée sur toute la course (pas de saut
 * au sortir de la zone morte, diagonales conservées). Axes SDL -32768..32767 (y vers le bas) -> Switch (y vers le haut). */
void stick(SDL_Gamepad *g, SDL_GamepadAxis ax, SDL_GamepadAxis ay, int32_t &x, int32_t &y) {
    const double fx = std::max(-1.0, SDL_GetGamepadAxis(g, ax) / 32767.0), fy = std::max(-1.0, -SDL_GetGamepadAxis(g, ay) / 32767.0);
    const double r = std::sqrt(fx * fx + fy * fy);
    if (r <= g_zone_morte || r == 0) { x = y = 0; return; }
    const double echelle = std::min(1.0, (r - g_zone_morte) / (1.0 - g_zone_morte)) / r;
    x = (int32_t)std::lround(std::clamp(fx * echelle, -1.0, 1.0) * 32767);
    y = (int32_t)std::lround(std::clamp(fy * echelle, -1.0, 1.0) * 32767);
}

Etat etat_manette(SDL_Gamepad *g) {
    Etat e;
    e.connecte = true;
    /* correspondance par étiquette (manette Xbox : A = A, B = B, X = X, Y = Y) ou par position (disposition Nintendo :
     * bouton du bas = B, de droite = A, de gauche = Y, du haut = X) */
    const bool n = g_disposition_nintendo;
    const struct { SDL_GamepadButton s; uint64_t b; } t[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, n ? B_B : B_A}, {SDL_GAMEPAD_BUTTON_EAST, n ? B_A : B_B}, {SDL_GAMEPAD_BUTTON_WEST, n ? B_Y : B_X},
        {SDL_GAMEPAD_BUTTON_NORTH, n ? B_X : B_Y}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, B_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, B_R},
        {SDL_GAMEPAD_BUTTON_START, B_PLUS}, {SDL_GAMEPAD_BUTTON_BACK, B_MINUS}, {SDL_GAMEPAD_BUTTON_LEFT_STICK, B_STICK_L},
        {SDL_GAMEPAD_BUTTON_RIGHT_STICK, B_STICK_R}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, B_GAUCHE}, {SDL_GAMEPAD_BUTTON_DPAD_UP, B_HAUT},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, B_DROITE}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, B_BAS}};
    for (auto &x : t)
        if (SDL_GetGamepadButton(g, x.s)) e.boutons |= x.b;
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000) e.boutons |= B_ZL;
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000) e.boutons |= B_ZR;
    stick(g, SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, e.lx, e.ly);
    stick(g, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, e.rx, e.ry);
    return e;
}

Etat etat_clavier() {
    Etat e;
    int n;
    const bool *k = SDL_GetKeyboardState(&n);
    static const struct { SDL_Scancode s; uint64_t b; } t[] = {
        {SDL_SCANCODE_RETURN, B_A}, {SDL_SCANCODE_BACKSPACE, B_B}, {SDL_SCANCODE_X, B_X}, {SDL_SCANCODE_Y, B_Y},
        {SDL_SCANCODE_Q, B_L}, {SDL_SCANCODE_E, B_R}, {SDL_SCANCODE_LSHIFT, B_ZL}, {SDL_SCANCODE_SPACE, B_ZR},
        {SDL_SCANCODE_P, B_PLUS}, {SDL_SCANCODE_M, B_MINUS}, {SDL_SCANCODE_LEFT, B_GAUCHE}, {SDL_SCANCODE_UP, B_HAUT},
        {SDL_SCANCODE_RIGHT, B_DROITE}, {SDL_SCANCODE_DOWN, B_BAS}};
    for (auto &x : t)
        if (x.s < n && k[x.s]) e.boutons |= x.b;
    if (k[SDL_SCANCODE_A]) e.lx = -32767;
    if (k[SDL_SCANCODE_D]) e.lx = 32767;
    if (k[SDL_SCANCODE_W]) e.ly = 32767;
    if (k[SDL_SCANCODE_S]) e.ly = -32767;
    return e;
}

/* directions des sticks en boutons (le jeu peut s'en servir dans les menus) */
void directions(Etat &e) {
    const int s = 16384;
    if (e.lx < -s) e.boutons |= B_LS_GAUCHE;
    if (e.lx > s) e.boutons |= B_LS_DROITE;
    if (e.ly > s) e.boutons |= B_LS_HAUT;
    if (e.ly < -s) e.boutons |= B_LS_BAS;
    if (e.rx < -s) e.boutons |= B_RS_GAUCHE;
    if (e.rx > s) e.boutons |= B_RS_DROITE;
    if (e.ry > s) e.boutons |= B_RS_HAUT;
    if (e.ry < -s) e.boutons |= B_RS_BAS;
}

void ecrire_lifo(uint32_t lifo, const Etat &e) {
    /* en-tête : {s64 horodatage, s64 nombre total (17), s64 queue, s64 nombre d'entrées} */
    uint32_t queue = (uint32_t)((rd64(lifo + 16) + 1) % N_ENTREES);
    uint64_t compte = rd64(lifo + 24);
    uint32_t ent = lifo + 0x20 + queue * TAILLE_ENTREE;
    wr64(ent, (uint64_t)g_echantillon << 1);          /* AtomicStorage::sampling_number */
    wr64(ent + 8, (uint64_t)g_echantillon);           /* NPadGenericState::sampling_number */
    wr64(ent + 16, e.boutons);
    wr32(ent + 24, (uint32_t)e.lx);
    wr32(ent + 28, (uint32_t)e.ly);
    wr32(ent + 32, (uint32_t)e.rx);
    wr32(ent + 36, (uint32_t)e.ry);
    wr32(ent + 40, e.connecte ? 0x1 | 0x2 : 0);       /* NpadAttribute : connecté, filaire */
    wr32(ent + 44, 0);
    wr64(lifo, (uint64_t)g_echantillon);
    wr64(lifo + 8, N_ENTREES);
    wr64(lifo + 16, queue);
    wr64(lifo + 24, compte < N_ENTREES - 1 ? compte + 1 : compte);
}

void ecrire_npad(uint32_t base_shmem, int i, const Etat &e) {
    uint32_t npad = base_shmem + NPAD_BASE + NPAD_TAILLE * i;
    wr32(npad + 0, e.connecte ? 1 : 0);   /* NpadStyleTag : FullKey (Pro Controller) */
    wr32(npad + 4, 0);                    /* NpadJoyAssignmentMode : Dual */
    wr32(npad + 8, e.connecte ? 0 : 2);   /* couleurs : attribut (0 = ok) */
    wr32(npad + 12, 0x2D2D2D);            /* corps */
    wr32(npad + 16, 0xE6E6E6);            /* boutons */
    ecrire_lifo(npad + LIFO_FULLKEY, e);
}

} // namespace

std::string config_texte(const char *section, const char *cle, const char *defaut); /* config.cpp (<exécutable>.ini) */
extern "C" int config_entier(const char *section, const char *cle, int defaut);
extern "C" int config_oui(const char *section, const char *cle, int defaut);

extern "C" void manettes_init(void) {
    g_t0 = SDL_GetTicks();
    g_zone_morte = std::clamp(config_entier("manettes", "zone_morte", 10), 0, 90) / 100.0;
    g_disposition_nintendo = config_texte("manettes", "disposition", "xbox") == "nintendo";
    g_vibration = config_oui("manettes", "vibration", 1);
    g_intensite_vibration = std::clamp(config_entier("manettes", "intensite_vibration", 100), 0, 200) / 100.0;
    fprintf(stderr, "[manettes] zone morte %.0f %%, disposition %s, vibration %s (%.0f %%)\n", g_zone_morte * 100,
            g_disposition_nintendo ? "nintendo" : "xbox", g_vibration ? "oui" : "non", g_intensite_vibration * 100);
    lire_script();
}

extern "C" void manettes_evenement(const SDL_Event *ev) {
    if (ev->type == SDL_EVENT_GAMEPAD_ADDED) {
        SDL_Gamepad *g = SDL_OpenGamepad(ev->gdevice.which);
        const int voulu = SDL_GetGamepadPlayerIndex(g); /* numéro XInput (LED de la manette) */
        if (voulu >= 0 && voulu < 8 && !g_manettes[voulu]) g_manettes[voulu] = g;
        else
            for (auto &m : g_manettes)
                if (!m) { m = g; break; }
        char guid[64];
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(ev->gdevice.which), guid, sizeof guid);
        const char *chemin = SDL_GetGamepadPath(g);
        fprintf(stderr, "[manettes] connectée : %s (id %u, type %d, joueur %d, chemin %s, guid %s)\n", SDL_GetGamepadName(g),
                (unsigned)ev->gdevice.which, (int)SDL_GetGamepadType(g), SDL_GetGamepadPlayerIndex(g), chemin ? chemin : "?", guid);
    } else if (ev->type == SDL_EVENT_GAMEPAD_REMOVED) {
        for (auto &m : g_manettes)
            if (m && SDL_GetGamepadID(m) == ev->gdevice.which) {
                std::lock_guard<std::mutex> lk(g_m_vib);
                SDL_CloseGamepad(m);
                m = nullptr;
            }
    }
}

/* appelée régulièrement par la boucle d'événements (thread principal) */
extern "C" void manettes_mettre_a_jour(void) {
    uint32_t base = svc_shmem_addr(g_hid_shmem);
    if (!base) return;
    g_echantillon++;
    double t = (SDL_GetTicks() - g_t0) / 1000.0;
    uint64_t script_j[8] = {};
    const double img = (double)rt_nb_presentations;
    for (auto &a : g_script)
        if (a.image ? img >= a.t && img < a.t + 6 : t >= a.t && t < a.t + 0.1) script_j[a.joueur] |= a.bouton;
    uint64_t script = 0;
    for (uint64_t b : script_j) script |= b; /* journal */
    static uint64_t script_prec = 0;
    static long n_maj = 0;
    n_maj++;
    if (script != script_prec) fprintf(stderr, "[manettes] t=%.2f s : script %llX (mises à jour : %ld)\n", t, (unsigned long long)script, n_maj);
    script_prec = script;
    /* joueurs contigus à partir de 1 : manettes dans l'ordre de leurs emplacements, puis joueurs scriptés virtuels */
    int joueur = 0;
    SDL_JoystickID ids[8] = {};
    for (auto *m : g_manettes) {
        if (!m) continue;
        Etat e = etat_manette(m);
        if (joueur == 0) { /* le clavier complète le joueur 1 */
            Etat k = etat_clavier();
            e.boutons |= k.boutons;
            if (k.lx) e.lx = k.lx;
            if (k.ly) e.ly = k.ly;
        }
        e.boutons |= script_j[joueur];
        directions(e);
        ids[joueur] = SDL_GetGamepadID(m);
        ecrire_npad(base, joueur++, e);
        if (joueur == 8) break;
    }
    while (joueur < g_joueurs_scriptes || joueur == 0) { /* clavier en joueur 1 sans manette ; joueurs scriptés virtuels */
        Etat e = joueur == 0 ? etat_clavier() : Etat{};
        e.connecte = true;
        e.boutons |= script_j[joueur];
        directions(e);
        ecrire_npad(base, joueur++, e);
    }
    Etat vide;
    for (int i = joueur; i < 8; i++) ecrire_npad(base, i, vide);
    std::lock_guard<std::mutex> lk(g_m_vib);
    memcpy(g_joueur_id, ids, sizeof ids);
}

/* hid : valeur de vibration pour une poignée VibrationDeviceHandle {u8 type, u8 npad_id, u8 appareil (0 gauche, 1 droit),
 * u8} ; amplitudes 0..1 des moteurs basse et haute fréquence. Moteurs Xbox : gros moteur (basses) <- amplitude basse,
 * petit moteur <- amplitude haute ; le plus fort des deux Joy-Con. La vibration Switch dure jusqu'à la valeur suivante :
 * envoyée à SDL pour 1 s et renouvelée tant qu'elle n'est pas nulle. */
extern "C" void manettes_vibrer(uint32_t poignee, float basse, float haute) {
    if (!g_vibration) return;
    const uint32_t npad = (poignee >> 8) & 0xFF, appareil = (poignee >> 16) & 1;
    const int joueur = npad == 0x20 ? 0 : (int)npad; /* 0x20 : mode portable = joueur 1 */
    if (joueur < 0 || joueur >= 8) return;
    std::lock_guard<std::mutex> lk(g_m_vib);
    g_vib[joueur][appareil][0] = std::isfinite(basse) ? basse : 0;
    g_vib[joueur][appareil][1] = std::isfinite(haute) ? haute : 0;
    auto moteur = [&](int k) {
        const double a = std::max(g_vib[joueur][0][k], g_vib[joueur][1][k]) * g_intensite_vibration;
        return (Uint16)std::lround(std::clamp(a, 0.0, 1.0) * 65535);
    };
    const Uint16 gros = moteur(0), petit = moteur(1);
    const Uint64 t = SDL_GetTicks();
    if (gros == g_vib_envoyee[joueur][0] && petit == g_vib_envoyee[joueur][1] && (gros + petit == 0 || t - g_vib_t[joueur] < 500)) return;
    SDL_Gamepad *g = g_joueur_id[joueur] ? SDL_GetGamepadFromID(g_joueur_id[joueur]) : nullptr;
    if (!g) return;
    SDL_RumbleGamepad(g, gros, petit, 1000);
    g_vib_envoyee[joueur][0] = gros;
    g_vib_envoyee[joueur][1] = petit;
    g_vib_t[joueur] = t;
}
