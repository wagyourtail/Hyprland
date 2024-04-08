#include "FrameSchedulingManager.hpp"
#include "../debug/Log.hpp"
#include "../Compositor.hpp"
#include "eventLoop/EventLoopManager.hpp"

static void onPresentTimer(std::shared_ptr<CEventLoopTimer> self, void* data) {
    g_pFrameSchedulingManager->onVblankTimer((CMonitor*)data);
}

static void onFenceTimer(std::shared_ptr<CEventLoopTimer> self, void* data) {
    g_pFrameSchedulingManager->onFenceTimer((CMonitor*)data);
}

void CFrameSchedulingManager::onFenceTimer(CMonitor* pMonitor) {
    SSchedulingData* DATA = &m_vSchedulingData.emplace_back(SSchedulingData{pMonitor});

    RASSERT(DATA, "No data in fenceTimer");

#ifndef GLES2
    GLint syncStatus = 0;
    glGetSynciv(DATA->fenceSync, GL_SYNC_STATUS, sizeof(GLint), nullptr, &syncStatus);
    bool GPUSignaled = syncStatus == GL_SIGNALED;

    if (GPUSignaled)
        gpuDone(pMonitor);
#endif
}

void CFrameSchedulingManager::registerMonitor(CMonitor* pMonitor) {
    if (dataFor(pMonitor)) {
        Debug::log(ERR, "BUG THIS: Attempted to double register to CFrameSchedulingManager");
        return;
    }

    SSchedulingData* DATA = &m_vSchedulingData.emplace_back(SSchedulingData{pMonitor});

#ifdef GLES2
    DATA->legacyScheduler = true;
#else
    DATA->legacyScheduler = !wlr_backend_is_drm(pMonitor->output->backend);
#endif

    DATA->fenceTimer  = std::make_shared<CEventLoopTimer>(::onFenceTimer, pMonitor);
    DATA->vblankTimer = std::make_shared<CEventLoopTimer>(::onPresentTimer, pMonitor);

    g_pEventLoopManager->addTimer(DATA->fenceTimer);
    g_pEventLoopManager->addTimer(DATA->vblankTimer);
}

void CFrameSchedulingManager::unregisterMonitor(CMonitor* pMonitor) {
    SSchedulingData* DATA = &m_vSchedulingData.emplace_back(SSchedulingData{pMonitor});
    g_pEventLoopManager->removeTimer(DATA->fenceTimer);
    g_pEventLoopManager->removeTimer(DATA->vblankTimer);
    std::erase_if(m_vSchedulingData, [pMonitor](const auto& d) { return d.pMonitor == pMonitor; });
}

void CFrameSchedulingManager::onFrameNeeded(CMonitor* pMonitor) {
    const auto DATA = dataFor(pMonitor);

    Debug::log(LOG, "onFrameNeeded");

    RASSERT(DATA, "No data in onFrameNeeded");

    if (pMonitor->tearingState.activelyTearing || DATA->legacyScheduler)
        return;

    if (DATA->activelyPushing && DATA->lastPresent.getMillis() < 100) {
        DATA->forceFrames = 1;
        return;
    }

    onPresent(pMonitor, nullptr);
}

void CFrameSchedulingManager::gpuDone(CMonitor* pMonitor) {
    const auto DATA = dataFor(pMonitor);

    Debug::log(LOG, "gpuDone");

    RASSERT(DATA, "No data in gpuDone");

    if (!DATA->delayed)
        return;

    Debug::log(LOG, "Missed a frame, rendering instantly");

    // delayed frame, let's render immediately, our shit will be presented soon
    // if we finish rendering before the next vblank somehow, kernel will be mad, but oh well
    g_pHyprRenderer->renderMonitor(DATA->pMonitor);
    DATA->delayedFrameSubmitted = true;
}

void CFrameSchedulingManager::registerBuffer(wlr_buffer* pBuffer, CMonitor* pMonitor) {
    const auto DATA = dataFor(pMonitor);

    Debug::log(LOG, "registerBuffer");

    RASSERT(DATA, "No data in registerBuffer");

    if (std::find(DATA->buffers.begin(), DATA->buffers.end(), pBuffer) != DATA->buffers.end())
        return;

    DATA->buffers.push_back(pBuffer);
}

void CFrameSchedulingManager::dropBuffer(wlr_buffer* pBuffer) {
    Debug::log(LOG, "dropBuffer");

    for (auto& d : m_vSchedulingData) {
        std::erase(d.buffers, pBuffer);
    }
}

void CFrameSchedulingManager::onFrame(CMonitor* pMonitor) {
    const auto DATA = dataFor(pMonitor);

    Debug::log(LOG, "onFrame");

    RASSERT(DATA, "No data in onFrame");

    if (!DATA->legacyScheduler)
        return;

    renderMonitor(DATA);
}

void CFrameSchedulingManager::onPresent(CMonitor* pMonitor, wlr_output_event_present* presentationData) {
    const auto DATA = dataFor(pMonitor);

    Debug::log(LOG, "onPresent");

    RASSERT(DATA, "No data in onPresent");

    if (pMonitor->tearingState.activelyTearing || DATA->legacyScheduler) {
        DATA->activelyPushing = false;
        return; // don't render
    }

    if (DATA->delayedFrameSubmitted) {
        DATA->delayedFrameSubmitted = false;
        return;
    }

    if (DATA->fenceSync) {
        glDeleteSync(DATA->fenceSync);
        DATA->fenceSync = nullptr;
    }

    Debug::log(LOG, "Present: del {}", DATA->delayed);

    int forceFrames = DATA->forceFrames + pMonitor->forceFullFrames;

    DATA->lastPresent.reset();

    // reset state, request a render if necessary
    DATA->delayed = false;
    if (DATA->forceFrames > 0)
        DATA->forceFrames--;
    DATA->rendered        = false;
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

    Debug::log(LOG, "render");

    uint64_t µsUntilVblank = 0;

    if (presentationData) {
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const std::chrono::system_clock::duration SINCELASTVBLANK{
            std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::seconds{now.tv_sec} + std::chrono::nanoseconds{now.tv_nsec}) -
            std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::seconds{presentationData->when->tv_sec} +
                                                                            std::chrono::nanoseconds{presentationData->when->tv_nsec})};
        µsUntilVblank = (presentationData->refresh ? presentationData->refresh / 1000.0 : pMonitor->refreshRate / 1000000.0) -
            std::chrono::duration_cast<std::chrono::microseconds>(SINCELASTVBLANK).count();

        DATA->nextVblank = std::chrono::system_clock::now() + std::chrono::microseconds{µsUntilVblank};
    } else
        µsUntilVblank = std::chrono::duration_cast<std::chrono::microseconds>(DATA->nextVblank - std::chrono::system_clock::now()).count();

    if (µsUntilVblank > 500)
        DATA->vblankTimer->updateTimeout(std::chrono::microseconds(µsUntilVblank - 500));

    Debug::log(LOG, "until vblank {}µs", µsUntilVblank);

    renderMonitor(DATA);
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

    data->fenceSync = g_pHyprRenderer->renderMonitor(pMonitor, !data->legacyScheduler);
    data->rendered  = true;
}

void CFrameSchedulingManager::onVblankTimer(CMonitor* pMonitor) {
    const auto DATA = dataFor(pMonitor);

    RASSERT(DATA, "No data in onVblankTimer");

    if (!DATA->rendered) {
        // what the fuck?
        Debug::log(ERR, "Vblank timer fired without a frame????");
        return;
    }

#ifndef GLES2

    g_pHyprRenderer->makeEGLCurrent();

    GLint syncStatus = 0;
    glGetSynciv(DATA->fenceSync, GL_SYNC_STATUS, sizeof(GLint), nullptr, &syncStatus);
    bool GPUSignaled = syncStatus == GL_SIGNALED;

    Debug::log(LOG, "vblank: signaled {}", GPUSignaled);

    if (GPUSignaled) {
        Debug::log(LOG, "timer nothing");
        // cool, we don't need to do anything. Wait for present.
        return;
    } else {
        Debug::log(LOG, "timer delay");
        // we missed a vblank :(
        DATA->delayed = true;
        // start the fence timer
        DATA->fenceTimer->updateTimeout(std::chrono::microseconds(850));
        return;
    }
#endif
}

bool CFrameSchedulingManager::isMonitorUsingLegacyScheduler(CMonitor* pMonitor) {
    const auto DATA = dataFor(pMonitor);

    if (!DATA)
        return true;

    return DATA->legacyScheduler;
}
