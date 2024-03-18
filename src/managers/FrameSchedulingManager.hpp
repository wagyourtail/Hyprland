#pragma once

#include <memory>
#include <vector>
#include "../helpers/Timer.hpp"

class CMonitor;
struct wlr_buffer;

class CFrameSchedulingManager {
  public:
    void registerMonitor(CMonitor* pMonitor);
    void unregisterMonitor(CMonitor* pMonitor);

    void gpuDone(wlr_buffer* pBuffer);
    void registerBuffer(wlr_buffer* pBuffer, CMonitor* pMonitor);
    void dropBuffer(wlr_buffer* pBuffer);

    void onFrameNeeded(CMonitor* pMonitor);

    void onPresent(CMonitor* pMonitor);

    int  onVblankTimer(void* data);

  private:
    struct SSchedulingData {
        CMonitor* pMonitor = nullptr;

        // CPU frame rendering has been finished
        bool rendered = false;

        // GPU frame rendering has been finished
        bool gpuReady = false;

        // GPU didn't manage to render last frame in time.
        // we got a vblank before we got a gpuDone()
        bool delayed = false;

        // whether the frame was submitted from gpuDone
        bool delayedFrameSubmitted = false;

        // don't plant a vblank timer
        bool noVblankTimer = false;

        // we need to render a few full frames at the beginning to catch all buffers
        int forceFrames = 5;

        // last present timer
        CTimer lastPresent;

        // buffers associated with this monitor
        std::vector<wlr_buffer*> buffers;

        // event source for the vblank timer
        wl_event_source* event = nullptr;

        // whether we're actively pushing frames
        bool activelyPushing = false;
    };

    std::vector<SSchedulingData> m_vSchedulingData;

    SSchedulingData*             dataFor(CMonitor* pMonitor);
    SSchedulingData*             dataFor(wlr_buffer* pBuffer);

    void                         renderMonitor(SSchedulingData* data);
};

inline std::unique_ptr<CFrameSchedulingManager> g_pFrameSchedulingManager;