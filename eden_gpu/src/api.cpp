// API C de eden_gpu.dll (cf. include/eden_gpu.h) : initialisation de l'émulation GPU d'Eden et accès nvdrv.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "hdr.h"
#include "interne.h"
#include "systeme_impl.h"
#include "core/frontend/framebuffer_layout.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "video_core/video_core.h"

namespace {

/// Fenêtre Win32 du portage, vue par le moteur de rendu Vulkan d'Eden (surface créée sur le HWND)
class FenetrePortage final : public Core::Frontend::EmuWindow {
public:
    FenetrePortage(void* hwnd, u32 largeur, u32 hauteur) {
        window_info.type = Core::Frontend::WindowSystemType::Windows;
        window_info.render_surface = hwnd;
        UpdateCurrentFramebufferLayout(largeur, hauteur);
    }
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<Core::Frontend::GraphicsContext>(); // Vulkan : contexte sans effet
    }
    bool IsShown() const override { return true; }
};

std::unique_ptr<Core::System> g_systeme;
std::unique_ptr<FenetrePortage> g_fenetre;
std::optional<Service::Nvidia::Module> g_module;
std::unique_ptr<Kernel::KProcess> g_processus;
Service::Nvidia::NvCore::SessionId g_session{};

/// Dossier « user » à côté de l'exécutable : Eden fonctionne alors en mode portable (journaux, cache de pipelines) et ne
/// touche pas à %APPDATA%\eden (configuration personnelle de l'utilisateur).
void preparer_dossier_portable() {
    wchar_t chemin[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, chemin, MAX_PATH);
    while (n && chemin[n - 1] != L'\\') --n;
    chemin[n] = 0;
    wcscat_s(chemin, L"user");
    CreateDirectoryW(chemin, nullptr);
    wcscat_s(chemin, L"\\cache"); // Eden ne crée pas les parents de son cache de shaders (user\cache\shader)
    CreateDirectoryW(chemin, nullptr);
}

} // namespace

namespace EdenGpu {
Core::System& Systeme() {
    return *g_systeme;
}
} // namespace EdenGpu

namespace EdenGpu {
u64 g_titre = 0; // identifiant du titre (eden_gpu_titre) : shim/core/hle/kernel/k_process.h
}

extern "C" {

namespace {
/// Réglages choisis par l'utilisateur (<exécutable>.ini), appliqués à l'initialisation après les défauts du portage
std::vector<std::pair<std::string, std::string>> g_options;

bool appliquer_option(const std::string& cle, const std::string& v) {
    using namespace Settings;
    auto& s = values;
    if (cle == "resolution_interne") {
        static const std::pair<const char*, ResolutionSetup> t[] = {
            {"0.25", ResolutionSetup::Res1_4X}, {"0.5", ResolutionSetup::Res1_2X}, {"0.75", ResolutionSetup::Res3_4X},
            {"1", ResolutionSetup::Res1X},      {"1.25", ResolutionSetup::Res5_4X}, {"1.5", ResolutionSetup::Res3_2X},
            {"2", ResolutionSetup::Res2X},      {"3", ResolutionSetup::Res3X},      {"4", ResolutionSetup::Res4X},
            {"5", ResolutionSetup::Res5X},      {"6", ResolutionSetup::Res6X},      {"7", ResolutionSetup::Res7X},
            {"8", ResolutionSetup::Res8X}};
        for (auto& [n, r] : t)
            if (v == n) { s.resolution_setup.SetValue(r); return true; }
        return false;
    }
    if (cle == "filtre") {
        static const std::pair<const char*, ScalingFilter> t[] = {
            {"plus_proche", ScalingFilter::NearestNeighbor}, {"bilineaire", ScalingFilter::Bilinear},
            {"bicubique", ScalingFilter::Bicubic},          {"gaussien", ScalingFilter::Gaussian},
            {"lanczos", ScalingFilter::Lanczos},            {"scaleforce", ScalingFilter::ScaleForce},
            {"fsr", ScalingFilter::Fsr},                    {"area", ScalingFilter::Area}};
        for (auto& [n, f] : t)
            if (v == n) { s.scaling_filter.SetValue(f); return true; }
        return false;
    }
    if (cle == "nettete_fsr") { s.fsr_sharpening_slider.SetValue(std::clamp(atoi(v.c_str()), 0, 200)); return true; }
    if (cle == "antialiasing") {
        if (v == "aucun") s.anti_aliasing.SetValue(AntiAliasing::None);
        else if (v == "fxaa") s.anti_aliasing.SetValue(AntiAliasing::Fxaa);
        else if (v == "smaa") s.anti_aliasing.SetValue(AntiAliasing::Smaa);
        else return false;
        return true;
    }
    if (cle == "anisotropie") {
        static const std::pair<const char*, AnisotropyMode> t[] = {
            {"auto", AnisotropyMode::Automatic}, {"defaut", AnisotropyMode::Default}, {"2", AnisotropyMode::X2},
            {"4", AnisotropyMode::X4},           {"8", AnisotropyMode::X8},           {"16", AnisotropyMode::X16}};
        for (auto& [n, a] : t)
            if (v == n) { s.max_anisotropy.SetValue(a); return true; }
        return false;
    }
    if (cle == "synchro_verticale") {
        if (v == "oui") s.vsync_mode.SetValue(VSyncMode::Fifo);
        else if (v == "non") s.vsync_mode.SetValue(VSyncMode::Immediate);
        else if (v == "mailbox") s.vsync_mode.SetValue(VSyncMode::Mailbox);
        else return false;
        return true;
    }
    if (cle == "hdr") { // sortie HDR10 (hdr.h) : effective si l'écran la propose
        if (v != "oui" && v != "non") return false;
        EdenHdr::R().demande = v == "oui";
        return true;
    }
    if (cle == "hdr_blanc") { EdenHdr::R().blanc = float(std::clamp(atoi(v.c_str()), 80, 1000)); return true; }
    if (cle == "hdr_crete") { EdenHdr::R().crete = float(std::clamp(atoi(v.c_str()), 200, 10000)); return true; }
    if (cle == "hdr_saturation") { EdenHdr::R().saturation = std::clamp(atoi(v.c_str()), 0, 200) / 100.0f; return true; }
    if (cle == "format_image") {
        static const std::pair<const char*, AspectRatio> t[] = {
            {"16_9", AspectRatio::R16_9}, {"4_3", AspectRatio::R4_3},   {"21_9", AspectRatio::R21_9},
            {"16_10", AspectRatio::R16_10}, {"etire", AspectRatio::Stretch}};
        for (auto& [n, a] : t)
            if (v == n) { s.aspect_ratio.SetValue(a); return true; }
        return false;
    }
    return false;
}
} // namespace

EDEN_API int eden_gpu_option(const char* cle, const char* valeur) {
    if (!cle || !valeur) return 0;
    g_options.emplace_back(cle, valeur);
    return 1;
}

EDEN_API void eden_gpu_titre(uint64_t title_id) {
    EdenGpu::g_titre = title_id;
}

EDEN_API int eden_gpu_init(void* hwnd, int largeur, int hauteur, uint8_t* memoire, const EdenRappels* rappels) {
    if (g_systeme) return 0;
    if (rappels) EdenGpu::g_rappels = *rappels;
    preparer_dossier_portable();

    setvbuf(stdout, nullptr, _IONBF, 0); // journal d'Eden sur stdout : rien de perdu si un ASSERT interrompt le processus
    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(true);

    // réglages (équivalents des défauts d'Eden, sauf GPU synchrone pour le prototype et décodage vidéo désactivé)
    Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Vulkan);
    // GPU asynchrone (défaut d'Eden) : en synchrone, le thread qui soumet attend la fin de chaque liste de commandes,
    // et une attente GPU sur une barrière incrémentée par une soumission suivante interbloque (NX_EDEN_GPU_SYNCHRONE=1).
    Settings::values.use_asynchronous_gpu_emulation.SetValue(getenv("NX_EDEN_GPU_SYNCHRONE") == nullptr);
    Settings::values.nvdec_emulation.SetValue(Settings::NvdecEmulation::Off);
    Settings::values.renderer_debug.SetValue(getenv("NX_EDEN_VALIDATION") != nullptr);
    Settings::SetCurrentProgramID(EdenGpu::g_titre);
    Settings::values.scaling_filter.SetValue(Settings::ScalingFilter::Bilinear); // défaut d'Eden : plus proche voisin
    for (auto& [cle, valeur] : g_options)
        if (!appliquer_option(cle, valeur)) EdenGpu::Journal("option d'affichage inconnue ou invalide : %s = %s", cle.c_str(), valeur.c_str());
    Settings::UpdateRescalingInfo(); // résolution interne

    g_systeme = std::make_unique<Core::System>();
    auto& impl = *g_systeme->impl;
    impl.device_memory.buffer.base = memoire;
    impl.memory.emplace(memoire);
    EdenGpu::DefinirBase(memoire);

    impl.host1x.emplace(*g_systeme); // 1) SMMU + points de synchronisation (avant le GPU)
    g_fenetre = std::make_unique<FenetrePortage>(hwnd, u32(largeur), u32(hauteur));
    VideoCore::CreateGPU(impl.gpu, *g_fenetre, *g_systeme); // 2) GPU + moteur Vulkan
    if (!impl.gpu) {
        EdenGpu::Journal("échec de la création du GPU émulé");
        return -1;
    }
    impl.gpu->Start(); // 3) thread GPU
    if (Settings::values.use_disk_shader_cache.GetValue()) { // 3b) cache de pipelines (build/cmake/user/cache/shader)
        const ULONGLONG t0 = GetTickCount64();
        impl.gpu->Renderer().ReadRasterizer()->LoadDiskResources(EdenGpu::g_titre, std::stop_token{},
                                                                 [](VideoCore::LoadCallbackStage, size_t, size_t) {});
        EdenGpu::Journal("cache de pipelines chargé en %llu ms", (unsigned long long)(GetTickCount64() - t0));
    }

    g_module.emplace(*g_systeme); // 4) périphériques nvdrv, session à mémoire identité
    g_processus = std::make_unique<Kernel::KProcess>(*impl.memory);
    g_session = g_module->GetContainer().OpenSession(g_processus.get());
    EdenGpu::AffichageInit(*g_systeme, *g_module); // 5) écran : files de tampons + compositeur (nvdisp_disp0)
    EdenGpu::Journal("GPU émulé prêt (%dx%d)", largeur, hauteur);
    return 0;
}

EDEN_API void eden_gpu_ecriture_hote(uint64_t adresse, uint64_t taille) {
    EdenGpu::EcritureHote(adresse, taille);
}

EDEN_API int eden_gpu_suivi_write_watch(void) {
    return EdenGpu::SuiviParWriteWatch();
}

EDEN_API void eden_gpu_fenetre(int largeur, int hauteur) {
    if (g_fenetre) g_fenetre->UpdateCurrentFramebufferLayout(u32(largeur), u32(hauteur));
}

EDEN_API void eden_gpu_arreter(void) {
    if (!g_systeme) return;
    auto& impl = *g_systeme->impl;
    impl.allume = false;
    EdenGpu::AffichageArreter(); // plus de composition (attend celle en cours)
    if (impl.gpu) {
        impl.gpu->NotifyShutdown(); // réveille les attentes de barrières
        impl.gpu.reset();           // thread GPU arrêté, périphérique Vulkan au repos, cache de pipelines écrit
    }
    EdenGpu::Journal("GPU émulé arrêté");
}

EDEN_API int eden_gpu_capture(const char* chemin, int largeur, int hauteur) {
    if (!g_systeme || !g_systeme->impl->gpu) return 0;
    auto& rendu = g_systeme->impl->gpu->Renderer();
    if (rendu.IsScreenshotPending()) return 0;
    const auto disposition = Layout::DefaultFrameLayout(u32(largeur), u32(hauteur));
    auto* pixels = new u8[size_t(largeur) * size_t(hauteur) * 4];
    std::string fichier{chemin};
    rendu.RequestScreenshot(
        pixels,
        [pixels, fichier, largeur, hauteur](bool inverse_y) {
            // BMP 32 bits : pixels BGRA du moteur de rendu ; lignes de bas en haut si inverse_y (hauteur positive)
            FILE* f = fopen(fichier.c_str(), "wb");
            if (f) {
                const u32 taille_px = u32(largeur) * u32(hauteur) * 4;
                u8 entete[54] = {'B', 'M'};
                auto ecrire32 = [&](int o, u32 v) { memcpy(entete + o, &v, 4); };
                ecrire32(2, 54 + taille_px);
                ecrire32(10, 54);
                ecrire32(14, 40);
                ecrire32(18, u32(largeur));
                ecrire32(22, inverse_y ? u32(hauteur) : u32(-hauteur));
                entete[26] = 1;
                entete[28] = 32;
                ecrire32(34, taille_px);
                fwrite(entete, 1, sizeof entete, f);
                fwrite(pixels, 1, taille_px, f);
                fclose(f);
            }
            delete[] pixels;
        },
        disposition);
    return 1;
}

EDEN_API int32_t eden_nvdrv_open(const char* chemin) {
    if (!g_module) return -1;
    return g_module->Open(chemin, g_session);
}

EDEN_API uint32_t eden_nvdrv_ioctl(int32_t fd, uint32_t commande, const uint8_t* in, size_t n_in, uint8_t* out, size_t n_out,
                                   const uint8_t* in2, size_t n_in2, uint8_t* out2, size_t n_out2, int variante) {
    if (!g_module) return u32(Service::Nvidia::NvResult::NotInitialized);
    Service::Nvidia::Ioctl cmd{};
    cmd.raw = commande;
    std::span<const u8> entree{in, n_in};
    std::span<u8> sortie{out, n_out};
    Service::Nvidia::NvResult r;
    if (variante == 2) r = g_module->Ioctl2(fd, cmd, entree, std::span<const u8>{in2, n_in2}, sortie);
    else if (variante == 3) r = g_module->Ioctl3(fd, cmd, entree, sortie, std::span<u8>{out2, n_out2});
    else r = g_module->Ioctl1(fd, cmd, entree, sortie);
    return u32(r);
}

EDEN_API uint32_t eden_nvdrv_close(int32_t fd) {
    if (!g_module) return u32(Service::Nvidia::NvResult::NotInitialized);
    return u32(g_module->Close(fd));
}

EDEN_API uint32_t eden_nvdrv_query_event(int32_t fd, uint32_t id, uint32_t* lisible) {
    if (!g_module) return u32(Service::Nvidia::NvResult::NotInitialized);
    Kernel::KEvent* ev = nullptr;
    auto r = g_module->QueryEvent(fd, id, ev);
    if (r == Service::Nvidia::NvResult::Success && ev && lisible) *lisible = ev->GetReadableEvent().handle;
    return u32(r);
}

} // extern "C"
