// Adaptateur : module nvdrv d'Eden sans le cadre de services du noyau (IPC assuré par le runtime natif).
// Même interface publique que src/core/hle/service/nvdrv/nvdrv.h (utilisée par les périphériques) ;
// implémentation : eden_gpu/src/nvdrv_module.cpp.
#pragma once

#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>

#include "common/common_types.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/nvdata.h"

namespace Core {
class System;
}

namespace Kernel {
class KEvent;
}

namespace Service::Nvidia {

namespace NvCore {
class Container;
class SyncpointManager;
} // namespace NvCore

namespace Devices {
class nvdevice;
class nvhost_ctrl;
} // namespace Devices

class Module;

class EventInterface {
public:
    explicit EventInterface(Module& module_);
    ~EventInterface();

    Kernel::KEvent* CreateEvent(std::string name);
    void FreeEvent(Kernel::KEvent* event);

private:
    Module& module;
    std::mutex guard;
};

class Module final {
public:
    explicit Module(Core::System& system_);
    ~Module();

    template <typename T>
    std::shared_ptr<T> GetDevice(DeviceFD fd) {
        std::scoped_lock lk{fichiers};
        auto itr = open_files.find(fd);
        if (itr == open_files.end())
            return nullptr;
        return std::static_pointer_cast<T>(itr->second);
    }

    NvResult VerifyFD(DeviceFD fd) const;
    DeviceFD Open(const std::string& device_name, NvCore::SessionId session_id);
    NvResult Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<u8> output);
    NvResult Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<const u8> inline_input,
                    std::span<u8> output);
    NvResult Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<u8> output,
                    std::span<u8> inline_output);
    NvResult Close(DeviceFD fd);
    NvResult QueryEvent(DeviceFD fd, u32 event_id, Kernel::KEvent*& event);

    NvCore::Container& GetContainer() {
        return container;
    }

private:
    friend class EventInterface;
    std::shared_ptr<Devices::nvdevice> Device(DeviceFD fd) const;

    NvCore::Container container;
    DeviceFD next_fd = 1;
    mutable std::mutex fichiers;
    std::unordered_map<DeviceFD, std::shared_ptr<Devices::nvdevice>> open_files;
    EventInterface events_interface;
    std::unordered_map<std::string, std::function<std::shared_ptr<Devices::nvdevice>()>> builders;
};

} // namespace Service::Nvidia
