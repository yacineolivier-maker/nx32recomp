// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Copie allégée pour le portage natif : sans les décodeurs matériels (nvdec, vic, FFmpeg) ni la file d'images,
// la vidéo passant par le décodeur natif du portage. Reste : points de synchronisation, SMMU, gestionnaire host1x.
#pragma once

#include "common/address_space.h"
#include "common/common_types.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/host1x/syncpoint_manager.h"
#include "video_core/memory_manager.h"

namespace Core {
class System;
} // namespace Core

namespace Tegra::Host1x {

enum class ChannelType : u32 {
    MsEnc = 0,
    VIC = 1,
    GPU = 2,
    NvDec = 3,
    Display = 4,
    NvJpg = 5,
    TSec = 6,
    Max = 7,
};

class Host1x {
public:
    explicit Host1x(Core::System& system);
    ~Host1x();

    Core::System& System() {
        return system;
    }
    SyncpointManager& GetSyncpointManager() {
        return syncpoint_manager;
    }
    const SyncpointManager& GetSyncpointManager() const {
        return syncpoint_manager;
    }
    Tegra::MaxwellDeviceMemoryManager& MemoryManager() {
        return memory_manager;
    }
    const Tegra::MaxwellDeviceMemoryManager& MemoryManager() const {
        return memory_manager;
    }
    Common::FlatAllocator<u32, 0, 32>& Allocator() {
        return allocator;
    }
    const Common::FlatAllocator<u32, 0, 32>& Allocator() const {
        return allocator;
    }

    Core::System& system;
    SyncpointManager syncpoint_manager;
    Tegra::MaxwellDeviceMemoryManager memory_manager;
    Tegra::MemoryManager gmmu_manager;
    Common::FlatAllocator<u32, 0, 32> allocator;
};

} // namespace Tegra::Host1x
