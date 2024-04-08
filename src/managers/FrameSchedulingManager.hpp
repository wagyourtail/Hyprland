#pragma once

#include <memory>
#include <vector>
#include "../helpers/Timer.hpp"
#include "eventLoop/EventLoopTimer.hpp"
#ifndef GLES2
#include <GLES3/gl32.h>
#else
#define GLsync void*
#endif

class CMonitor;
struct wlr_buffer;
struct wlr_output_event_present;

class CFrameSchedulingManager {
  public:
    void registerMonitor(CMonitor* pMonitor);
    void unregisterMonitor(CMonitor* pMonitor);

    void gpuDone(CMonitor* pMonitor);
    void registerBuffer(wlr_buffer* pBuffer, CMonitor* pMonitor);
    void dropBuffer(wlr_buffer* pBuffer);
    void onFenceTimer(CMonitor* pMonitor);

    void onFrameNeeded(CMonitor* pMonitor);

    void onPresent(CMonitor* pMonitor, wlr_output_event_present* presentationData);
    void onFrame(CMonitor* pMonitor);

    void onVblankTimer(void* data);

    bool isMonitorUsingLegacyScheduler(CMonitor* pMonitor);

  private:
    struct SSchedulingData {
        CMonitor* pMonitor = nullptr;

        // CPU frame rendering has been finished
        bool rendered = false;

        // GPU didn't manage to render last frame in time.
        // we got a vblank before we got a gpuDone()
        bool delayed = false;

        // whether the frame was submitted from gpuDone
        bool delayedFrameSubmitted = false;

        // we need to render a few full frames at the beginning to catch all buffers
        int forceFrames = 5;

        // last present timer
        CTimer lastPresent;

        // buffers associated with this monitor
        std::vector<wlr_buffer*> buffers;

        // whether we're actively pushing frames
        bool activelyPushing = false;

        // legacy scheduler: for backends that do not send us reliable present events
        // these rely on onFrame
        bool legacyScheduler = false;

        // next predicted vblank
        std::chrono::system_clock::time_point nextVblank;

        // for delayed fence stuff
        std::shared_ptr<CEventLoopTimer> fenceTimer;
        std::shared_ptr<CEventLoopTimer> vblankTimer;

        // fence sync
        GLsync fenceSync = 0;
    };

    std::vector<SSchedulingData> m_vSchedulingData;

    SSchedulingData*             dataFor(CMonitor* pMonitor);
    SSchedulingData*             dataFor(wlr_buffer* pBuffer);

    void                         renderMonitor(SSchedulingData* data);
};

inline std::unique_ptr<CFrameSchedulingManager> g_pFrameSchedulingManager;