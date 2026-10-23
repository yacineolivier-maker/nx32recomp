// Adaptateur : statistiques de performance (compteur d'images composées).
#pragma once

#include <atomic>

#include "common/common_types.h"

namespace Core {

class PerfStats {
public:
    void EndGameFrame() { ++images; }
    void BeginSystemFrame() {}
    void EndSystemFrame() {}
    std::atomic<u64> images{0};
};

} // namespace Core
