#include "FrameSchedulingManager.hpp"
#include "../debug/Log.hpp"
#include "../Compositor.hpp"

int onPresentTimer(void* data) {
    return g_pFrameSchedulingManager->onVblankTimer(data);
}

void CFrameSchedulingManager::registerMonitor(CMonitor* pMonitor) {
    if (dataFor(pMonitor)) {
        Debug::log(ERR, "BUG THIS: Attempted to double register to CFrameSchedulingManager");
        return;
    }

    SSchedulingData* DATA = &m_vSchedulingData.emplace_back(SSchedulingData{pMonitor});
    DATA->event           = wl_event_loop_add_timer(g_pCompositor->m_sWLEventLoop, onPresentTimer, DATA);

    DATA->legacyScheduler = !wlr_backend_is_drm(pMonitor->output->backend);
}

void CFrameSchedulingManager::unregisterMonitor(CMonitor* pMonitor) {
    std::erase_if(m_vSchedulingData, [pMonitor](const auto& d) { return d.pMonitor == pMonitor; });
}

void CFrameSchedulingManager::onFrameNeeded(CMonitor* pMonitor) {
    const auto DATA = dataFor(pMonitor);

    RASSERT(DATA, "No data in onFrameNeeded");

    if (pMonitor->tearingState.activelyTearing || DATA->legacyScheduler)
        return;

    if (DATA->activelyPushing && DATA->lastPresent.getMillis() < 100) {
        if (DATA->forceFrames < 1)
            DATA->forceFrames++;
        return;
    }

    DATA->noVblankTimer = true;
    onPresent(pMonitor, nullptr);
}

void CFrameSchedulingManager::gpuDone(wlr_buffer* pBuffer) {
    const auto DATA = dataFor(pBuffer);

    RASSERT(DATA, "No data in gpuDone");

    if (!DATA->delayed)
        return;

    // delayed frame, let's render immediately, our shit will be presented soon
    // if we finish rendering before the next vblank somehow, kernel will be mad, but oh well
    DATA->noVblankTimer = true;
    g_pHyprRenderer->renderMonitor(DATA->pMonitor);
    DATA->delayedFrameSubmitted = true;
}

void CFrameSchedulingManager::registerBuffer(wlr_buffer* pBuffer, CMonitor* pMonitor) {
    const auto DATA = dataFor(pMonitor);

    RASSERT(DATA, "No data in registerBuffer");

    if (std::find(DATA->buffers.begin(), DATA->buffers.end(), pBuffer) != DATA->buffers.end())
        return;

    DATA->buffers.push_back(pBuffer);
}

void CFrameSchedulingManager::dropBuffer(wlr_buffer* pBuffer) {
    for (auto& d : m_vSchedulingData) {
        std::erase(d.buffers, pBuffer);
    }
}

void CFrameSchedulingManager::onFrame(CMonitor* pMonitor) {
    const auto DATA = dataFor(pMonitor);

    RASSERT(DATA, "No data in onFrame");

    if (!DATA->legacyScheduler)
        return;

    renderMonitor(DATA);
}

void CFrameSchedulingManager::onPresent(CMonitor* pMonitor, wlr_output_event_present* presentationData) {
    const auto DATA = dataFor(pMonitor);

    RASSERT(DATA, "No data in onPresent");

    if (pMonitor->tearingState.activelyTearing || DATA->legacyScheduler) {
        DATA->activelyPushing = false;
        return; // don't render
    }

    if (DATA->delayedFrameSubmitted) {
        DATA->delayedFrameSubmitted = false;
        DATA->activelyPushing       = false;
        return;
    }

    int forceFrames = DATA->forceFrames + pMonitor->forceFullFrames;

    DATA->lastPresent.reset();

    // reset state, request a render if necessary
    DATA->delayed = false;
    if (DATA->forceFrames > 0)
        DATA->forceFrames--;
    DATA->rendered        = false;
    DATA->gpuReady        = false;
    DATA->activelyPushing = true;

    // check if there is damage
    bool hasDamage = pixman_region32_not_empty(&pMonitor->damage.current);
    if (!hasDamage) {
        for (int i = 0; i < WLR_DAMAGE_RING_PREVIOUS_LEN; ++i) {
            hasDamage = hasDamage || pixman_region32_not_empty(&pMonitor->damage.previous[i]);
        }
    }

    if (!hasDamage && forceFrames <= 0) {
        DATA->activelyPushing = false;
        return;
    }

    // we can't do this on wayland
    if (!wlr_backend_is_wl(pMonitor->output->backend) && !DATA->noVblankTimer && presentationData) {
        const std::chrono::system_clock::time_point LASTVBLANK{std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::seconds{presentationData->when->tv_sec} + std::chrono::nanoseconds{presentationData->when->tv_nsec})};
        const float                                 MSUNTILVBLANK = (presentationData->refresh ? presentationData->refresh / 1000000.0 : pMonitor->refreshRate / 1000.0) -
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - LASTVBLANK).count();
        wl_event_source_timer_update(DATA->event, 0);
        wl_event_source_timer_update(DATA->event, std::floor(MSUNTILVBLANK));
    }

    renderMonitor(DATA);

    DATA->noVblankTimer = false;
}

CFrameSchedulingManager::SSchedulingData* CFrameSchedulingManager::dataFor(CMonitor* pMonitor) {
    for (auto& d : m_vSchedulingData) {
        if (d.pMonitor == pMonitor)
            return &d;
    }

    return nullptr;
}

CFrameSchedulingManager::SSchedulingData* CFrameSchedulingManager::dataFor(wlr_buffer* pBuffer) {
    for (auto& d : m_vSchedulingData) {
        if (std::find(d.buffers.begin(), d.buffers.end(), pBuffer) != d.buffers.end())
            return &d;
    }

    return nullptr;
}

void CFrameSchedulingManager::renderMonitor(SSchedulingData* data) {
    CMonitor* pMonitor = data->pMonitor;

    if ((g_pCompositor->m_sWLRSession && !g_pCompositor->m_sWLRSession->active) || !g_pCompositor->m_bSessionActive || g_pCompositor->m_bUnsafeState) {
        Debug::log(WARN, "Attempted to render frame on inactive session!");

        if (g_pCompositor->m_bUnsafeState && std::ranges::any_of(g_pCompositor->m_vMonitors.begin(), g_pCompositor->m_vMonitors.end(), [&](auto& m) {
                return m->output != g_pCompositor->m_pUnsafeOutput->output;
            })) {
            // restore from unsafe state
            g_pCompositor->leaveUnsafeState();
        }

        return; // cannot draw on session inactive (different tty)
    }

    if (!pMonitor->m_bEnabled)
        return;

    g_pHyprRenderer->recheckSolitaryForMonitor(pMonitor);

    pMonitor->tearingState.busy = false;

    if (pMonitor->tearingState.activelyTearing && pMonitor->solitaryClient /* can be invalidated by a recheck */) {

        if (!pMonitor->tearingState.frameScheduledWhileBusy)
            return; // we did not schedule a frame yet to be displayed, but we are tearing. Why render?

        pMonitor->tearingState.nextRenderTorn          = true;
        pMonitor->tearingState.frameScheduledWhileBusy = false;
    }

    g_pHyprRenderer->renderMonitor(pMonitor);
    data->rendered = true;
}

int CFrameSchedulingManager::onVblankTimer(void* data) {
    auto DATA = (SSchedulingData*)data;

    if (DATA->rendered && DATA->gpuReady) {
        // cool, we don't need to do anything. Wait for present.
        return 0;
    }

    if (DATA->rendered && !DATA->gpuReady) {
        // we missed a vblank :(
        DATA->delayed = true;
        return 0;
    }

    // what the fuck?
    Debug::log(ERR, "Vblank timer fired without a frame????");
    return 0;
}

bool CFrameSchedulingManager::isMonitorUsingLegacyScheduler(CMonitor* pMonitor) {
    const auto DATA = dataFor(pMonitor);

    if (!DATA)
        return true;

    return DATA->legacyScheduler;
}
