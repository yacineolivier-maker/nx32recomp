// Sortie HDR10 (cf. hdr.h) : passe plein écran image du jeu (SDR) -> image de la chaîne d'échange (HDR10).
#include "hdr.h"

#include <algorithm>
#include <array>
#include <atomic>

#include "hdr_frag_spv.h" // généré depuis shaders/hdr.frag (glslangValidator, cf. CMakeLists.txt)
#include "hdr_vert_spv.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace EdenHdr {

namespace {
Reglages g_reglages;
std::atomic<bool> g_actif{false};
constexpr VkFormat FORMAT_HDR = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
constexpr u32 MAX_FRAMES = 16; // Frame::index < MAX_FRAMES_IN_FLIGHT (8)

struct Constantes {
    float blanc, crete, saturation, seuil;
};
} // namespace

Reglages& R() {
    return g_reglages;
}
bool Actif() {
    return g_actif.load();
}
void DefinirActif(bool actif) {
    g_actif.store(actif);
}

Passe::Passe(const Vulkan::Device& device_) : device{device_} {
    using namespace Vulkan;
    vert = CreateWrappedShaderModule(device, HDR_VERT_SPV);
    frag = CreateWrappedShaderModule(device, HDR_FRAG_SPV);
    set_layout = CreateWrappedDescriptorSetLayout(device, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER}, VK_SHADER_STAGE_FRAGMENT_BIT);
    const VkPushConstantRange constantes{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(Constantes),
    };
    layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &constantes,
    });
    render_pass = CreateWrappedRenderPass(device, FORMAT_HDR, VK_IMAGE_LAYOUT_UNDEFINED); // finit en GENERAL
    pipeline = CreateWrappedPipeline(device, render_pass, layout, std::tie(vert, frag));
    sampler = CreateBilinearSampler(device);
    pool = CreateWrappedDescriptorPool(device, MAX_FRAMES, MAX_FRAMES);
    std::array<VkDescriptorSetLayout, MAX_FRAMES> layouts;
    layouts.fill(*set_layout);
    sets = CreateWrappedDescriptorSets(pool, layouts);
}

Passe::~Passe() = default;

/// Vues et framebuffers des images de la chaîne d'échange (recréés quand la chaîne change)
void Passe::PreparerCibles(Vulkan::Swapchain& swapchain) {
    const VkExtent2D extent = swapchain.GetExtent();
    // recréation de la chaîne : Invalider() a vidé les cibles (comparer les handles ne suffit pas, ils peuvent être réutilisés)
    if (!cibles.empty()) return;
    cibles.resize(swapchain.GetImageCount());
    extent_cibles = extent;
    for (size_t i = 0; i < cibles.size(); i++) {
        Cible& c = cibles[i];
        c.image = swapchain.GetImageIndex(i);
        c.vue = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = c.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = FORMAT_HDR,
            .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY},
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        });
        c.framebuffer = Vulkan::CreateWrappedFramebuffer(device, render_pass, c.vue, extent);
    }
}

void Passe::Enregistrer(vk::CommandBuffer cmdbuf, Vulkan::Frame& frame, Vulkan::Swapchain& swapchain) {
    PreparerCibles(swapchain);
    Cible& cible = cibles[swapchain.GetImageIndex()];

    // ensemble de descripteurs de cette Frame, réécrit à chaque image (sa vue peut avoir été recréée avec la même
    // valeur de handle) : la Frame n'est réutilisée qu'après la fin de sa présentation précédente (GetRenderFrame
    // attend present_done), son ensemble n'est donc plus lu par le GPU
    const u32 n = frame.index % MAX_FRAMES;
    std::vector<VkDescriptorImageInfo> infos;
    infos.reserve(1);
    const VkWriteDescriptorSet ecriture = Vulkan::CreateWriteDescriptorSet(infos, *sampler, *frame.image_view, sets[n], 0);
    device.GetLogical().UpdateDescriptorSets(std::array{ecriture}, {});

    // image du jeu : écrite comme attachement de couleur (GENERAL), lue ici par le shader de fragments
    const VkImageMemoryBarrier lecture{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = *frame.image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, lecture);

    Vulkan::BeginRenderPass(cmdbuf, *render_pass, *cible.framebuffer, extent_cibles);
    cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline);
    const VkDescriptorSet set = sets[n];
    cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, *layout, 0, set, {});
    const Reglages& r = g_reglages;
    const Constantes c{std::min(r.blanc, r.crete), r.crete, r.saturation, 0.5f}; // blanc au plus égal à la crête
    cmdbuf.PushConstants(*layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof c, &c);
    cmdbuf.Draw(3, 1, 0, 0);
    cmdbuf.EndRenderPass();

    // image de la chaîne : GENERAL (fin de la passe) -> présentation
    const VkImageMemoryBarrier presentation{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = cible.image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, presentation);
}

} // namespace EdenHdr
