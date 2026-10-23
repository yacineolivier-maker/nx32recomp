/* Services système HLE (ajoutés au fur et à mesure que le jeu les sollicite).
 * Numéros de commandes : https://switchbrew.org/wiki/ (Applet Manager services, Log services, PPC services…). */
#include "ipc.h"

#include <atomic>

#include <cstdio>
#include <deque>
#include <mutex>

std::shared_ptr<Service> applet_storage(std::vector<uint8_t> data);
std::vector<uint8_t> launch_param_preselected_user();
std::shared_ptr<Service> library_applet_creator(); /* applets.cpp */

#include "jeu.h"

uint64_t langue_code_systeme(); /* settings.cpp ([systeme] langue) */

namespace {

using Fn = TableService::Fn;
std::shared_ptr<TableService> make(const char *name, std::initializer_list<std::pair<const uint32_t, Fn>> cmds) {
    auto s = std::make_shared<TableService>(name);
    for (auto &kv : cmds) s->cmds.insert(kv);
    return s;
}
void ok(Request &, Response &) {}
template <class T> Fn ret(T v) { return [v](Request &, Response &rs) { rs.push<T>(v); }; }
Fn obj(std::function<std::shared_ptr<Service>()> f) {
    return [f](Request &, Response &rs) { rs.objects.push_back(f()); };
}

/* ------------------------------------------------------------------ lm */
std::shared_ptr<Service> lm_logger() {
    return make("lm:ILogger", {
        {0, [](Request &rq, Response &) { /* Log : paquet binaire, on extrait le texte imprimable */
             Buf b = rq.in();
             std::string s;
             for (uint32_t i = 0; i < b.size; i++) {
                 char ch = (char)rd8(b.addr + i);
                 if (ch >= 0x20 && ch < 0x7F) s += ch;
                 else if (!s.empty() && s.back() != ' ') s += ' ';
             }
             fprintf(stderr, "[lm] %s\n", s.c_str());
         }},
        {1, ok}, /* SetDestination */
    });
}

/* ------------------------------------------------------------------ applet (appletOE) */
struct AppletState {
    std::mutex m;
    std::deque<uint32_t> messages{15}; /* 15 = FocusStateChanged (l'application démarre au premier plan) */
    uint32_t event_w = 0, event_r = 0;
    void init() {
        if (!event_w) {
            event_w = svc_new_event(1, &event_r);
        }
    }
} g_applet;

/* Focus de l'application (1 = au premier plan, 2 = hors focus) : comme Eden, une applet de bibliothèque au premier plan
 * fait perdre le focus à l'application puis le lui rend, chaque fois avec le message FocusStateChanged (15) */
} // namespace

std::atomic<uint8_t> g_focus{1};
void applet_message(uint32_t message) {
    g_applet.init();
    std::lock_guard<std::mutex> lk(g_applet.m);
    g_applet.messages.push_back(message);
    svc_signal_event(g_applet.event_w);
}
void applet_focus(uint8_t etat) {
    g_focus = etat;
    applet_message(15);
}

namespace {

std::shared_ptr<Service> common_state_getter() {
    return make("am:ICommonStateGetter", {
        {0, [](Request &, Response &rs) { g_applet.init(); rs.copy_handles.push_back(g_applet.event_r); }}, /* GetEventHandle */
        {1, [](Request &, Response &rs) { /* ReceiveMessage */
             /* comme Eden (AppletMessageQueue::PopMessage) : l'événement « nouveau message » est effacé quand la file
              * est vide. Resté signalé, il réveillait en permanence le thread applet du jeu (priorité 41, cœur 0), qui
              * monopolisait le cœur et affamait le thread principal et les travaux de rendu de ce cœur. */
             g_applet.init();
             std::lock_guard<std::mutex> lk(g_applet.m);
             if (g_applet.messages.empty()) { svc_clear_event(g_applet.event_w); rs.result = 0x680; return; }
             rs.push<uint32_t>(g_applet.messages.front());
             g_applet.messages.pop_front();
             if (g_applet.messages.empty()) svc_clear_event(g_applet.event_w);
         }},
        {5, ret<uint8_t>(1)},  /* GetOperationMode : 1 = sur station (docked) */
        {6, ret<uint32_t>(1)}, /* GetPerformanceMode : 1 = Boost (docked) */
        {8, ret<uint8_t>(0)},  /* GetBootMode */
        {9, [](Request &, Response &rs) { rs.push<uint8_t>(g_focus.load()); }}, /* GetCurrentFocusState */
        {60, [](Request &, Response &rs) { rs.push<uint32_t>(jeu_largeur_station()); rs.push<uint32_t>(1080); }}, /* GetDefaultDisplayResolution */
    });
}

std::shared_ptr<Service> self_controller() {
    auto s = make("am:ISelfController", {
        {0, ok}, {1, ok}, {2, ok}, {3, ok}, {4, ok},
        {9, [](Request &, Response &rs) { /* GetLibraryAppletLaunchableEvent : signalé (comme Eden) ; sans handle, le SDK
                                             attendait sur le handle 0 en boucle active (applet de sélection des manettes) */
             static uint32_t lisible = 0;
             if (!lisible) svc_new_event(1, &lisible);
             rs.copy_handles.push_back(lisible);
         }},
        {10, ok}, {11, ok}, {12, ok}, {13, ok}, {14, ok}, {16, ok}, {17, ok}, {18, ok}, {19, ok},
        {40, ret<uint64_t>(1)}, /* CreateManagedDisplayLayer -> identifiant de couche */
        {50, ok}, {51, ok}, {62, ok}, {65, ok}, {68, ok},
    });
    return s;
}

std::shared_ptr<Service> window_controller() {
    return make("am:IWindowController", {
        {1, ret<uint64_t>(0x1234)}, /* GetAppletResourceUserId */
        {10, ok},                   /* AcquireForegroundRights */
    });
}

std::shared_ptr<Service> application_functions() {
    return make("am:IApplicationFunctions", {
        {1, [](Request &rq, Response &rs) { /* PopLaunchParameter(kind) : l'utilisateur présélectionné, une seule fois */
             static bool donne = false;
             uint32_t kind = rq.pop<uint32_t>();
             if (kind == 2 && !donne) {
                 donne = true;
                 rs.objects.push_back(applet_storage(launch_param_preselected_user()));
             } else {
                 rs.result = 0x2A2;
             }
         }},
        {20, ret<uint64_t>(0)},                                  /* EnsureSaveData */
        {21, [](Request &, Response &rs) { rs.push<uint64_t>(langue_code_systeme()); }}, /* GetDesiredLanguage */
        {22, ok}, /* SetTerminateResult */
        {23, [](Request &, Response &rs) { rs.push<uint64_t>(0); rs.push<uint64_t>(0); }}, /* GetDisplayVersion */
        {40, ret<uint8_t>(0)}, /* NotifyRunning */
        {50, [](Request &, Response &rs) { rs.push<uint64_t>(0); rs.push<uint64_t>(0); }}, /* GetPseudoDeviceId */
        {66, ok}, {67, ok}, {68, ok}, /* enregistrement vidéo / RequestFlushGamePlayingMovieForDebug */
        {100, ok}, {101, ok},       /* InitializeGamePlayRecording / SetGamePlayRecordingState */
        {130, [](Request &, Response &rs) { uint32_t r; rs.copy_handles.push_back((svc_new_event(0, &r), r)); }},
    });
}

std::shared_ptr<Service> application_proxy() {
    return make("am:IApplicationProxy", {
        {0, obj(common_state_getter)},
        {1, obj(self_controller)},
        {2, obj(window_controller)},
        {3, obj([] { return make("am:IAudioController", {{0, ok}, {1, ret<float>(1.0f)}, {2, ret<float>(1.0f)}, {3, ok}, {4, ok}}); })},
        {4, obj([] { return make("am:IDisplayController", {}); })},
        {11, obj(library_applet_creator)},
        {20, obj(application_functions)},
        {1000, obj([] { return make("am:IDebugFunctions", {}); })},
    });
}

} // namespace

void fs_register();
void vi_register();
void hid_register();
void account_register();
void settings_register();
void time_register();
void mii_register();
void audio_register();
void friend_register();
void pl_register();
void nvdrv_register();

void services_init() {
    pl_register();
    nvdrv_register(); /* pilote NVN d'origine du jeu : périphériques GPU vers l'émulation d'Eden */
    mii_register();
    audio_register();
    friend_register();
    account_register();
    settings_register();
    time_register();
    fs_register();
    vi_register();
    hid_register();
    ipc_register("lm", [] { return make("lm", {{0, obj(lm_logger)}}); });
    ipc_register("apm", [] {
        return make("apm", {
            {0, obj([] { return make("apm:ISession", {{0, ok}, {1, ret<uint32_t>(0x00010000)}}); })},
            {1, ret<uint32_t>(1)}, /* GetPerformanceMode */
        });
    });
    ipc_register("appletOE", [] { return make("appletOE", {{0, obj(application_proxy)}}); });

    /* contrôle parental : aucune restriction */
    auto pctl = [] {
        auto svc = obj([] {
            return make("pctl:IParentalControlService", {
                {1, ok}, {1001, ok}, {1002, ok}, {1003, ok}, {1004, ok}, {1006, ret<uint8_t>(0)},
                {1013, ok}, {1017, ok}, {1018, ok},
                {1031, ret<uint8_t>(0)}, {1032, ret<uint32_t>(0)}, {1033, ok}, {1034, ok},
                {1035, ret<uint8_t>(0)}, {1036, ret<uint8_t>(0)},
            });
        });
        return make("pctl", {{0, svc}, {1, svc}});
    };
    for (auto n : {"pctl", "pctl:a", "pctl:s", "pctl:r"}) ipc_register(n, pctl);

    /* contenus additionnels : aucun */
    ipc_register("aoc:u", [] {
        return make("aoc:u", {
            {0, ret<uint32_t>(0)}, {2, ret<uint32_t>(0)}, {3, ret<uint32_t>(0)},
            {5, [](Request &, Response &rs) { rs.push<uint64_t>(jeu_title_id() + 0x1000); }}, {7, ok}, /* titre de base des DLC */
            {8, [](Request &, Response &rs) { uint32_t r; svc_new_event(0, &r); rs.copy_handles.push_back(r); }},
        });
    });
}
