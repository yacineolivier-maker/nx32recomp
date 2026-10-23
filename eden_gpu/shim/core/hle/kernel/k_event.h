// Adaptateur : événement noyau d'Eden relié aux événements du runtime natif (svc). Signal()/Clear() peuvent être
// appelés depuis les threads d'Eden (GPU, attente des barrières) : le runtime les rend sûrs entre threads.
#pragma once

#include "common/common_types.h"

namespace Kernel {

class KernelCore {};

class KReadableEvent {
public:
    u32 handle = 0; ///< handle lisible côté jeu (copié par nvdrv QueryEvent)
};

class KEvent {
public:
    KEvent(u32 handle_ecriture, u32 handle_lecture) : ecriture{handle_ecriture} { lisible.handle = handle_lecture; }
    void Signal(KernelCore&) { Signal(); }
    void Clear(KernelCore&) { Clear(); }
    void Signal();
    void Clear();
    KReadableEvent& GetReadableEvent() { return lisible; }
    u32 HandleEcriture() const { return ecriture; }

private:
    u32 ecriture;
    KReadableEvent lisible;
};

} // namespace Kernel
