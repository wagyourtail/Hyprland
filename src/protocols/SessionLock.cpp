#include "SessionLock.hpp"
#include "../Compositor.hpp"
#include "FractionalScale.hpp"

#define LOGM PROTO::sessionLock->protoLog

CSessionLockSurface::CSessionLockSurface(SP<CExtSessionLockSurfaceV1> resource_, wlr_surface* surface_, CMonitor* pMonitor_, WP<CSessionLock> owner_) :
    resource(resource_), sessionLock(owner_), pSurface(surface_), pMonitor(pMonitor_) {
    if (!resource->resource())
        return;

    resource->setDestroy([this](CExtSessionLockSurfaceV1* r) {
        events.destroy.emit();
        PROTO::sessionLock->destroyResource(this);
    });
    resource->setOnDestroy([this](CExtSessionLockSurfaceV1* r) {
        events.destroy.emit();
        PROTO::sessionLock->destroyResource(this);
    });

    resource->setAckConfigure([this](CExtSessionLockSurfaceV1* r, uint32_t serial) { ackdConfigure = true; });

    hyprListener_surfaceCommit.initCallback(
        &pSurface->events.commit,
        [this](void* owner, void* data) {
            if (pSurface->pending.buffer_width <= 0 || pSurface->pending.buffer_height <= 0) {
                LOGM(ERR, "SessionLock attached a null buffer");
                resource->error(EXT_SESSION_LOCK_SURFACE_V1_ERROR_NULL_BUFFER, "Null buffer attached");
                return;
            }

            if (!ackdConfigure) {
                LOGM(ERR, "SessionLock committed without an ack");
                resource->error(EXT_SESSION_LOCK_SURFACE_V1_ERROR_COMMIT_BEFORE_FIRST_ACK, "Committed surface before first ack");
                return;
            }

            if (committed)
                events.commit.emit();
            else {
                wlr_surface_map(pSurface);
                events.map.emit();
            }
            committed = true;
        },
        this, "SessionLockSurface");

    hyprListener_surfaceDestroy.initCallback(
        &pSurface->events.destroy,
        [this](void* owner, void* data) {
            LOGM(WARN, "SessionLockSurface object remains but surface is being destroyed???");
            wlr_surface_unmap(pSurface);
            hyprListener_surfaceCommit.removeCallback();
            hyprListener_surfaceDestroy.removeCallback();

            if (g_pCompositor->m_pLastFocus == pSurface)
                g_pCompositor->m_pLastFocus = nullptr;

            pSurface = nullptr;
        },
        this, "SessionLockSurface");

    PROTO::fractional->sendScale(surface_, pMonitor_->scale);

    sendConfigure();

    listeners.monitorMode = pMonitor->events.modeChanged.registerListener([this](std::any data) { sendConfigure(); });
}

CSessionLockSurface::~CSessionLockSurface() {
    if (pSurface && pSurface->mapped)
        wlr_surface_unmap(pSurface);
    hyprListener_surfaceCommit.removeCallback();
    hyprListener_surfaceDestroy.removeCallback();
    events.destroy.emit(); // just in case.
}

void CSessionLockSurface::sendConfigure() {
    const auto SERIAL = wlr_seat_client_next_serial(wlr_seat_client_for_wl_client(g_pCompositor->m_sSeat.seat, resource->client()));
    resource->sendConfigure(SERIAL, pMonitor->vecSize.x, pMonitor->vecSize.y);
}

bool CSessionLockSurface::good() {
    return resource->resource();
}

bool CSessionLockSurface::inert() {
    return sessionLock.expired();
}

CMonitor* CSessionLockSurface::monitor() {
    return pMonitor;
}

wlr_surface* CSessionLockSurface::surface() {
    return pSurface;
}

CSessionLock::CSessionLock(SP<CExtSessionLockV1> resource_) : resource(resource_) {
    if (!resource->resource())
        return;

    resource->setDestroy([this](CExtSessionLockV1* r) { PROTO::sessionLock->destroyResource(this); });
    resource->setOnDestroy([this](CExtSessionLockV1* r) { PROTO::sessionLock->destroyResource(this); });

    resource->setGetLockSurface([this](CExtSessionLockV1* r, uint32_t id, wl_resource* surf, wl_resource* output) {
        if (inert) {
            LOGM(ERR, "Lock is trying to send getLockSurface after it's inert");
            return;
        }

        PROTO::sessionLock->onGetLockSurface(r, id, surf, output);
    });

    resource->setUnlockAndDestroy([this](CExtSessionLockV1* r) {
        events.unlockAndDestroy.emit();
        inert                      = true;
        PROTO::sessionLock->locked = false;
        PROTO::sessionLock->destroyResource(this);
    });
}

CSessionLock::~CSessionLock() {
    events.destroyed.emit();
}

void CSessionLock::sendLocked() {
    resource->sendLocked();
}

bool CSessionLock::good() {
    return resource->resource();
}

CSessionLockProtocol::CSessionLockProtocol(const wl_interface* iface, const int& ver, const std::string& name) : IWaylandProtocol(iface, ver, name) {
    ;
}

void CSessionLockProtocol::bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) {
    const auto RESOURCE = m_vManagers.emplace_back(std::make_unique<CExtSessionLockManagerV1>(client, ver, id)).get();
    RESOURCE->setOnDestroy([this](CExtSessionLockManagerV1* p) { this->onManagerResourceDestroy(p->resource()); });

    RESOURCE->setDestroy([this](CExtSessionLockManagerV1* pMgr) { this->onManagerResourceDestroy(pMgr->resource()); });
    RESOURCE->setLock([this](CExtSessionLockManagerV1* pMgr, uint32_t id) { this->onLock(pMgr, id); });
}

void CSessionLockProtocol::onManagerResourceDestroy(wl_resource* res) {
    std::erase_if(m_vManagers, [&](const auto& other) { return other->resource() == res; });
}

void CSessionLockProtocol::destroyResource(CSessionLock* lock) {
    std::erase_if(m_vLocks, [&](const auto& other) { return other.get() == lock; });
}

void CSessionLockProtocol::destroyResource(CSessionLockSurface* surf) {
    std::erase_if(m_vLockSurfaces, [&](const auto& other) { return other.get() == surf; });
}

void CSessionLockProtocol::onLock(CExtSessionLockManagerV1* pMgr, uint32_t id) {

    LOGM(LOG, "New sessionLock with id {}", id);

    const auto CLIENT   = pMgr->client();
    const auto RESOURCE = m_vLocks.emplace_back(std::make_unique<CSessionLock>(std::make_shared<CExtSessionLockV1>(CLIENT, pMgr->version(), id)));

    if (!RESOURCE->good()) {
        pMgr->noMemory();
        m_vLocks.pop_back();
        return;
    }

    if (m_vLocks.size() > 1) {
        LOGM(ERR, "Tried to lock a locked session");
        RESOURCE->resource->sendFinished();
        RESOURCE->inert = true;
        return;
    }

    events.newLock.emit(RESOURCE);

    locked = true;
}

void CSessionLockProtocol::onGetLockSurface(CExtSessionLockV1* lock, uint32_t id, wl_resource* surface, wl_resource* output) {
    LOGM(LOG, "New sessionLockSurface with id {}", id);

    auto             PSURFACE = wlr_surface_from_resource(surface);
    auto             PMONITOR = g_pCompositor->getMonitorFromOutput(wlr_output_from_resource(output));

    SP<CSessionLock> sessionLock;
    for (auto& l : m_vLocks) {
        if (l->resource.get() == lock) {
            sessionLock = l;
            break;
        }
    }

    const auto RESOURCE = m_vLockSurfaces.emplace_back(
        std::make_unique<CSessionLockSurface>(std::make_shared<CExtSessionLockSurfaceV1>(lock->client(), lock->version(), id), PSURFACE, PMONITOR, sessionLock));

    if (!RESOURCE->good()) {
        lock->noMemory();
        m_vLockSurfaces.pop_back();
        return;
    }

    sessionLock->events.newLockSurface.emit(RESOURCE);
}

bool CSessionLockProtocol::isLocked() {
    return locked;
}