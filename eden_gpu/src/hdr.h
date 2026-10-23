// Sortie HDR10 : chaîne d'échange A2B10G10R10 / HDR10_ST2084 (patch/vk_swapchain.cpp) et passe de conversion de l'image
// SDR du jeu vers PQ/BT.2020 au moment de la copie vers l'écran (patch/vk_present_manager.cpp). Réglages : <exécutable>.ini.
#pragma once

#include <memory>
#include <vector>

#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
class Device;
class Swapchain;
struct Frame;
} // namespace Vulkan

namespace EdenHdr {

namespace vk = Vulkan::vk;

struct Reglages {
    bool demande = false;    // [affichage] hdr = oui
    float blanc = 203.0f;    // nits du blanc de l'image du jeu
    float crete = 1000.0f;   // nits maximum de l'écran
    float saturation = 1.0f; // 1 = couleurs d'origine
};
Reglages& R();

// vrai si la chaîne d'échange a été créée en HDR10 (format et espace proposés par l'écran / Windows)
bool Actif();
void DefinirActif(bool actif);

// Passe de conversion : dessine l'image du jeu (Frame, SDR B8G8R8A8) dans l'image courante de la chaîne d'échange
class Passe {
public:
    explicit Passe(const Vulkan::Device& device);
    ~Passe();
    // à enregistrer après l'acquisition de l'image ; laisse l'image de la chaîne en PRESENT_SRC
    void Enregistrer(vk::CommandBuffer cmdbuf, Vulkan::Frame& frame, Vulkan::Swapchain& swapchain);
    // à appeler, GPU au repos, avant la recréation de la chaîne d'échange (handles d'images réutilisables par le pilote)
    void Invalider() { cibles.clear(); }

private:
    struct Cible {
        VkImage image{};
        vk::ImageView vue;
        vk::Framebuffer framebuffer;
    };
    void PreparerCibles(Vulkan::Swapchain& swapchain);

    const Vulkan::Device& device;
    vk::ShaderModule vert, frag;
    vk::DescriptorSetLayout set_layout;
    vk::PipelineLayout layout;
    vk::RenderPass render_pass;
    vk::Pipeline pipeline;
    vk::Sampler sampler;
    vk::DescriptorPool pool;
    vk::DescriptorSets sets;
    std::vector<Cible> cibles;
    VkExtent2D extent_cibles{};
};

} // namespace EdenHdr
