// Affichage (vi) : files de tampons Android d'Eden (nvnflinger : IGraphicBufferProducer côté jeu, consommateur côté
// composition) et compositeur matériel (nvdisp_disp0 -> GPU::RequestComposite -> présentation Vulkan sur la fenêtre).
// Tient lieu de SurfaceFlinger, HosBinderDriverServer et Conductor d'Eden, réduits à un écran et ses couches ; la cadence
// (vsync) est donnée par le runtime, qui appelle eden_vi_composer à chaque période.
#include <map>
#include <memory>
#include <mutex>
#include <span>

#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/devices/nvdisp_disp0.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/hle/service/nvnflinger/buffer_item_consumer.h"
#include "core/hle/service/nvnflinger/buffer_queue_consumer.h"
#include "core/hle/service/nvnflinger/buffer_queue_core.h"
#include "core/hle/service/nvnflinger/buffer_queue_producer.h"
#include "core/hle/service/nvnflinger/display.h"
#include "core/hle/service/nvnflinger/hardware_composer.h"
#include "interne.h"

namespace Service::KernelHelpers {

ServiceContext::ServiceContext(Core::System& system_, std::string) : kernel{system_.Kernel()} {}
ServiceContext::~ServiceContext() = default;

Kernel::KEvent* ServiceContext::CreateEvent(std::string&&) {
    u32 lisible = 0, ecriture = 0;
    if (EdenGpu::g_rappels.evenement_creer) ecriture = EdenGpu::g_rappels.evenement_creer(EdenGpu::g_rappels.ctx, &lisible);
    return new Kernel::KEvent(ecriture, lisible);
}

void ServiceContext::CloseEvent(Kernel::KEvent* event) {
    delete event;
}

} // namespace Service::KernelHelpers

namespace {

using namespace Service;

/// Un écran (id 0) et ses couches ; m protège la pile de couches, le compositeur et la table des binders
struct Affichage {
    Affichage(Core::System& systeme, Nvidia::Module& module_) : module{module_}, contexte{systeme, "vi"}, ecran{0} {
        disp_fd = module.Open("/dev/nvdisp_disp0", {});
    }

    Nvidia::Module& module;
    KernelHelpers::ServiceContext contexte;
    Nvnflinger::Display ecran;
    Nvnflinger::HardwareComposer composeur;
    Nvidia::DeviceFD disp_fd{};
    std::map<s32, std::shared_ptr<android::IBinder>> binders;
    std::map<s32, s32> consommateur_de; ///< id producteur -> id consommateur (couche)
    s32 prochain_id = 1;
    bool arrete = false; ///< arrêt du GPU : plus de composition
    std::mutex m;
};

std::unique_ptr<Affichage> g_affichage;

std::shared_ptr<android::IBinder> binder_de(s32 id) {
    if (!g_affichage) return nullptr;
    std::scoped_lock lk{g_affichage->m};
    auto it = g_affichage->binders.find(id);
    return it == g_affichage->binders.end() ? nullptr : it->second;
}

} // namespace

namespace EdenGpu {

void AffichageInit(Core::System& systeme, Service::Nvidia::Module& module) {
    g_affichage = std::make_unique<Affichage>(systeme, module);
}

void AffichageArreter() {
    if (!g_affichage) return;
    std::scoped_lock lk{g_affichage->m};
    g_affichage->arrete = true;
}

} // namespace EdenGpu

extern "C" {

EDEN_API int32_t eden_vi_creer_couche(void) {
    if (!g_affichage) return -1;
    auto& a = *g_affichage;
    auto& nvmap = a.module.GetContainer().GetNvMapFile();
    auto coeur = std::make_shared<android::BufferQueueCore>();
    auto producteur = std::make_shared<android::BufferQueueProducer>(a.contexte, coeur, nvmap);
    auto consommateur = std::make_shared<android::BufferQueueConsumer>(coeur);
    auto conso_items = std::make_shared<android::BufferItemConsumer>(consommateur);
    conso_items->Connect(false);

    std::scoped_lock lk{a.m};
    const s32 id_conso = a.prochain_id++;
    const s32 id_prod = a.prochain_id++;
    a.binders.emplace(id_conso, consommateur);
    a.binders.emplace(id_prod, producteur);
    a.consommateur_de[id_prod] = id_conso;
    a.ecran.stack.layers.emplace_back(std::make_shared<Nvnflinger::Layer>(std::move(conso_items), id_conso));
    EdenGpu::Journal("couche %d créée (producteur %d)", id_conso, id_prod);
    return id_prod;
}

EDEN_API void eden_vi_detruire_couche(int32_t producteur) {
    if (!g_affichage) return;
    auto& a = *g_affichage;
    std::scoped_lock lk{a.m};
    auto it = a.consommateur_de.find(producteur);
    if (it == a.consommateur_de.end()) return;
    const s32 id_conso = it->second;
    a.composeur.RemoveLayerLocked(a.ecran, id_conso);
    std::erase_if(a.ecran.stack.layers, [&](auto& couche) { return couche->consumer_id == id_conso; });
    a.binders.erase(id_conso);
    a.binders.erase(producteur);
    a.consommateur_de.erase(it);
}

EDEN_API int eden_vi_transact(int32_t binder, uint32_t code, const uint8_t* in, size_t n_in, uint8_t* out, size_t n_out,
                              uint32_t flags) {
    auto b = binder_de(binder);
    if (!b) return 0;
    // hors verrou : DequeueBuffer peut attendre qu'un tampon soit rendu par la composition
    b->Transact(code, std::span<const u8>{in, n_in}, std::span<u8>{out, n_out}, flags);
    return 1;
}

EDEN_API int eden_vi_evenement(int32_t binder, uint32_t type, uint32_t* lisible) {
    auto b = binder_de(binder);
    if (!b) return 0;
    auto* ev = b->GetNativeHandle(type);
    if (!ev) return 0;
    *lisible = ev->handle;
    return 1;
}

EDEN_API int eden_vi_composer(void) {
    if (!g_affichage) return 0;
    auto& a = *g_affichage;
    std::scoped_lock lk{a.m};
    if (a.arrete || !a.ecran.stack.HasLayers()) return 0;
    auto disp = a.module.GetDevice<Nvidia::Devices::nvdisp_disp0>(a.disp_fd);
    if (!disp) return 0;
    f32 echelle = 1.0f;
    return s32(a.composeur.ComposeLocked(&echelle, a.ecran, *disp));
}

} // extern "C"
