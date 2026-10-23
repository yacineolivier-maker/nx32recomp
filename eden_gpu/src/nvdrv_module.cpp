// Module nvdrv : périphériques d'Eden (nvmap, nvhost-as-gpu, nvhost-gpu, nvhost-ctrl, nvhost-ctrl-gpu, nvdisp_disp0)
// sans le cadre de services du noyau (d'après src/core/hle/service/nvdrv/nvdrv.cpp). Les décodeurs matériels (nvdec,
// vic, nvjpg) sont exclus : la vidéo passe par le décodeur natif du portage.
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/devices/nvdevice.h"
#include "core/hle/service/nvdrv/devices/nvdisp_disp0.h"
#include "core/hle/service/nvdrv/devices/nvhost_as_gpu.h"
#include "core/hle/service/nvdrv/devices/nvhost_ctrl.h"
#include "core/hle/service/nvdrv/devices/nvhost_ctrl_gpu.h"
#include "core/hle/service/nvdrv/devices/nvhost_gpu.h"
#include "core/hle/service/nvdrv/devices/nvmap.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "interne.h"

namespace Service::Nvidia {

EventInterface::EventInterface(Module& module_) : module{module_} {}
EventInterface::~EventInterface() = default;

Kernel::KEvent* EventInterface::CreateEvent(std::string name) {
    uint32_t lisible = 0, ecriture = 0;
    if (EdenGpu::g_rappels.evenement_creer) ecriture = EdenGpu::g_rappels.evenement_creer(EdenGpu::g_rappels.ctx, &lisible);
    EdenGpu::Journal("événement %s : écriture %08X lecture %08X", name.c_str(), ecriture, lisible);
    return new Kernel::KEvent(ecriture, lisible);
}

void EventInterface::FreeEvent(Kernel::KEvent* event) {
    delete event; // les handles du runtime restent valides (le jeu peut encore les détenir)
}

Module::Module(Core::System& system) : container{system.Host1x()}, events_interface{*this} {
    builders["/dev/nvhost-as-gpu"] = [this, &system] { return std::make_shared<Devices::nvhost_as_gpu>(system, *this, container); };
    builders["/dev/nvhost-gpu"] = [this, &system] { return std::make_shared<Devices::nvhost_gpu>(system, events_interface, container); };
    builders["/dev/nvhost-ctrl-gpu"] = [this, &system] { return std::make_shared<Devices::nvhost_ctrl_gpu>(system, events_interface); };
    builders["/dev/nvmap"] = [this, &system] { return std::make_shared<Devices::nvmap>(system, container); };
    builders["/dev/nvdisp_disp0"] = [this, &system] { return std::make_shared<Devices::nvdisp_disp0>(system, container); };
    builders["/dev/nvhost-ctrl"] = [this, &system] { return std::make_shared<Devices::nvhost_ctrl>(system, events_interface, container); };
}

Module::~Module() = default;

std::shared_ptr<Devices::nvdevice> Module::Device(DeviceFD fd) const {
    std::scoped_lock lk{fichiers};
    auto it = open_files.find(fd);
    return it == open_files.end() ? nullptr : it->second;
}

NvResult Module::VerifyFD(DeviceFD fd) const {
    if (fd < 0) return NvResult::InvalidState;
    return Device(fd) ? NvResult::Success : NvResult::NotImplemented;
}

DeviceFD Module::Open(const std::string& device_name, NvCore::SessionId session_id) {
    auto it = builders.find(device_name);
    if (it == builders.end()) return INVALID_NVDRV_FD;
    std::shared_ptr<Devices::nvdevice> device = it->second();
    DeviceFD fd;
    {
        std::scoped_lock lk{fichiers};
        fd = next_fd++;
        open_files.emplace(fd, device);
    }
    device->OnOpen(session_id, fd);
    return fd;
}

NvResult Module::Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<u8> output) {
    if (fd < 0) return NvResult::InvalidState;
    auto d = Device(fd);
    return d ? d->Ioctl1(fd, command, input, output) : NvResult::NotImplemented;
}

NvResult Module::Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<const u8> inline_input,
                        std::span<u8> output) {
    if (fd < 0) return NvResult::InvalidState;
    auto d = Device(fd);
    return d ? d->Ioctl2(fd, command, input, inline_input, output) : NvResult::NotImplemented;
}

NvResult Module::Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<u8> output,
                        std::span<u8> inline_output) {
    if (fd < 0) return NvResult::InvalidState;
    auto d = Device(fd);
    return d ? d->Ioctl3(fd, command, input, output, inline_output) : NvResult::NotImplemented;
}

NvResult Module::Close(DeviceFD fd) {
    if (fd < 0) return NvResult::InvalidState;
    std::shared_ptr<Devices::nvdevice> d;
    {
        std::scoped_lock lk{fichiers};
        auto it = open_files.find(fd);
        if (it == open_files.end()) return NvResult::NotImplemented;
        d = it->second;
        open_files.erase(it);
    }
    d->OnClose(fd);
    return NvResult::Success;
}

NvResult Module::QueryEvent(DeviceFD fd, u32 event_id, Kernel::KEvent*& event) {
    if (fd < 0) return NvResult::InvalidState;
    auto d = Device(fd);
    if (!d) return NvResult::NotImplemented;
    event = d->QueryEvent(event_id);
    return event ? NvResult::Success : NvResult::BadParameter;
}

} // namespace Service::Nvidia
