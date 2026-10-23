/* Fenêtre SDL3 et boucle d'événements (thread principal du programme). */
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "jeu.h"

extern "C" void manettes_init(void);
extern "C" void manettes_evenement(const SDL_Event *ev);
extern "C" void manettes_mettre_a_jour(void);
extern "C" int eden_demarrer(void *hwnd, int largeur, int hauteur, int hauteur_ecran);
extern "C" void eden_fenetre(int largeur, int hauteur);
extern "C" int config_entier(const char *section, const char *cle, int defaut); /* config.cpp (<exécutable>.ini) */
extern "C" int config_oui(const char *section, const char *cle, int defaut);
extern std::string g_donnees_dir; /* fs.cpp */
static SDL_Window *g_fenetre = nullptr;

extern "C" int gfx_demarrer(void) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        fprintf(stderr, "[gfx] SDL_Init : %s\n", SDL_GetError());
        return 0;
    }
    const int largeur = config_entier("fenetre", "largeur", 1280), hauteur = config_entier("fenetre", "hauteur", 720);
    SDL_WindowFlags options = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
    /* plein écran par défaut (fenêtré : plein_ecran = non, ou NX_FENETRE=1 pour les tests automatiques) */
    if (config_oui("fenetre", "plein_ecran", 1) && !getenv("NX_FENETRE")) options |= SDL_WINDOW_FULLSCREEN;
    SDL_Window *w = SDL_CreateWindow(jeu_nom(), largeur, hauteur, options);
    if (!w) {
        fprintf(stderr, "[gfx] fenêtre : %s\n", SDL_GetError());
        return 0;
    }
    manettes_init();
    g_fenetre = w;
    if (options & SDL_WINDOW_FULLSCREEN) SDL_HideCursor(); /* curseur masqué en plein écran */
    { /* icône de la fenêtre (icône du jeu, copiée dans le dossier des données) */
        if (SDL_Surface *icone = SDL_LoadBMP((g_donnees_dir + "/icone.bmp").c_str())) {
            SDL_SetWindowIcon(w, icone);
            SDL_DestroySurface(icone);
        }
    }
    /* le GPU émulé d'Eden présente lui-même sur la fenêtre (une seule swapchain par HWND) */
    void *hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    int lp = 1280, hp = 720;
    SDL_GetWindowSizeInPixels(w, &lp, &hp);
    const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(w));
    return eden_demarrer(hwnd, lp, hp, mode ? mode->h : hp);
}

/* Boucle d'événements : rend la main quand la fenêtre est fermée.
 * NX_DUREE=s (tests) : fermeture demandée au bout de s secondes, qui passe par l'arrêt propre (GPU arrêté avant la fin
 * du processus) au lieu d'un arrêt brutal pendant un appel au pilote graphique (processus zombies). */
extern "C" void gfx_boucle(void) {
    if (const char *d = getenv("NX_DUREE")) {
        SDL_CreateThread([](void *p) -> int {
            SDL_Delay((Uint32)(uintptr_t)p * 1000);
            SDL_Event q{};
            q.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&q);
            return 0;
        }, "duree", (void *)(uintptr_t)atoi(d));
    }
    for (;;) {
        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 4)) {
            if (e.type == SDL_EVENT_QUIT) return;
            manettes_evenement(&e);
            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) eden_fenetre(e.window.data1, e.window.data2);
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F11) {
                bool plein = (SDL_GetWindowFlags(g_fenetre) & SDL_WINDOW_FULLSCREEN) != 0;
                SDL_SetWindowFullscreen(g_fenetre, !plein);
                if (plein) SDL_ShowCursor();
                else SDL_HideCursor();
            }
        }
        manettes_mettre_a_jour();
    }
}
