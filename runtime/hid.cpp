/* hid : manettes. La mémoire partagée (0x40000 octets) est remplie par le runtime (SDL3, à venir).
 * Interfaces : https://switchbrew.org/wiki/HID_services */
#include "ipc.h"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <vector>

uint32_t g_hid_shmem;
extern "C" void manettes_vibrer(uint32_t poignee, float basse, float haute); /* manettes.cpp */ /* handle de la mémoire partagée HID (adresse : svc_shmem_addr) */

namespace {

void ok(Request &, Response &) {}

/* VibrationValue : {f32 amplitude basse, f32 fréquence basse, f32 amplitude haute, f32 fréquence haute} */
struct ValeurVibration { float amp_basse, freq_basse, amp_haute, freq_haute; };

void vibrer(uint32_t poignee, const ValeurVibration &v) { manettes_vibrer(poignee, v.amp_basse, v.amp_haute); }

std::shared_ptr<Service> applet_resource() {
    auto s = std::make_shared<TableService>("hid:IAppletResource");
    s->cmds[0] = [](Request &, Response &rs) { rs.copy_handles.push_back(g_hid_shmem); }; /* GetSharedMemoryHandle */
    return s;
}

uint32_t event_lecture() {
    uint32_t r;
    svc_new_event(1, &r);
    return r;
}

} // namespace

/* contenu initial de la mémoire partagée : en-têtes des files circulaires (nombre d'entrées) comme le service réel.
 * Une file à 0 entrée fait boucler le lecteur du jeu (modulo par 0 dans la division logicielle du SDK) : blocage
 * sporadique dans nn::hid::GetTouchScreenStates. En-tête : {u64 horodatage, u64 nb_entrées, u64 dernière, u64 compte}. */
const uint8_t *hid_image_initiale() {
    static std::vector<uint8_t> img(0x40000, 0);
    auto entete = [](uint32_t off, uint64_t n) { memcpy(img.data() + off + 8, &n, 8); };
    entete(0x0, 17);      /* DebugPad */
    entete(0x400, 17);    /* TouchScreen */
    entete(0x3400, 17);   /* Mouse */
    entete(0x3800, 17);   /* Keyboard */
    for (int i = 0; i < 4; i++) entete(0x3C00 + 0x400 * i, 17); /* BasicXpad */
    entete(0x4C00, 17);   /* HomeButton */
    entete(0x4E00, 17);   /* SleepButton */
    entete(0x5000, 17);   /* CaptureButton */
    for (int i = 0; i < 16; i++) entete(0x5200 + 0x80 * i, 2); /* InputDetector */
    entete(0x3BA00, 17);  /* Gesture */
    for (int p = 0; p < 10; p++) { /* Npad : 7 files de touches (0x350) puis 6 files de capteurs de mouvement (0x708) */
        const uint32_t b = 0x9A00 + 0x5000 * p;
        for (int k = 0; k < 7; k++) entete(b + 0x28 + 0x350 * k, 17);
        for (int k = 0; k < 6; k++) entete(b + 0x1758 + 0x708 * k, 17);
    }
    return img.data();
}

void hid_register() {
    g_hid_shmem = svc_new_shmem(0x40000);
    svc_shmem_set_init(g_hid_shmem, hid_image_initiale());
    ipc_register("hid", [] {
        auto s = std::make_shared<TableService>("hid");
        s->cmds[0] = [](Request &, Response &rs) { rs.objects.push_back(applet_resource()); }; /* CreateAppletResource */
        s->cmds[101] = [](Request &, Response &rs) { rs.push<uint32_t>(0x1F); };            /* GetSupportedNpadStyleSet */
        s->cmds[106] = [](Request &, Response &rs) { rs.copy_handles.push_back(event_lecture()); }; /* AcquireNpadStyleSetUpdateEventHandle */
        s->cmds[108] = [](Request &, Response &rs) { rs.push<uint64_t>(1); };               /* GetPlayerLedPattern */
        s->cmds[121] = [](Request &, Response &rs) { rs.push<uint64_t>(1); };               /* GetNpadJoyHoldType : horizontal */
        s->cmds[200] = [](Request &rq, Response &rs) { /* GetVibrationDeviceInfo : actionneur linéaire, Joy-Con gauche/droit */
            const uint32_t poignee = rq.pop<uint32_t>();
            rs.push<uint32_t>(1);
            rs.push<uint32_t>(((poignee >> 16) & 1) + 1);
        };
        s->cmds[201] = [](Request &rq, Response &) { /* SendVibrationValue(poignée, valeur, aruid) */
            const uint32_t poignee = rq.pop<uint32_t>();
            vibrer(poignee, rq.pop<ValeurVibration>());
        };
        s->cmds[203] = [](Request &, Response &rs) {                                         /* CreateActiveVibrationDeviceList */
            auto l = std::make_shared<TableService>("hid:IActiveVibrationDeviceList");
            l->cmds[0] = ok;
            rs.objects.push_back(l);
        };
        s->cmds[206] = [](Request &rq, Response &) { /* SendVibrationValues(aruid ; X0 poignées, X1 valeurs) */
            const Buf p = rq.in(0), v = rq.in(1);
            const uint32_t n = std::min(p.size / 4, v.size / (uint32_t)sizeof(ValeurVibration));
            for (uint32_t i = 0; i < n && i < 16; i++) {
                ValeurVibration val;
                for (uint32_t k = 0; k < 4; k++) {
                    const uint32_t mot = rd32(v.addr + i * 16 + k * 4);
                    memcpy(reinterpret_cast<char *>(&val) + k * 4, &mot, 4);
                }
                vibrer(rd32(p.addr + i * 4), val);
            }
        };
        for (uint32_t c : {11u, 21u, 31u, 66u, 67u, 100u, 102u, 103u, 104u, 120u, 122u, 123u, 124u, 125u, 128u, 204u})
            s->cmds[c] = ok; /* Activate*, SetSupportedNpad*, SetNpadJoyHoldType, StartSixAxisSensor, modes : sans effet */
        s->cmds[211] = [](Request &, Response &rs) { rs.push<uint8_t>(1); }; /* IsVibrationDeviceMounted */
        return s;
    });
}
