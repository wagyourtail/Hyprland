#pragma once

#include "Framebuffer.hpp"

class CMonitor;

class CRenderbuffer {
  public:
    CRenderbuffer(wlr_buffer* buffer, uint32_t format, CMonitor* pMonitor);
    ~CRenderbuffer();

    void          bind();
    void          bindFB();
    void          unbind();
    CFramebuffer* getFB();
    void          plantFence();
    void          removeFence();

    wlr_buffer*   m_pWlrBuffer = nullptr;

    DYNLISTENER(destroyBuffer);

  private:
    EGLImageKHR      m_iImage = 0;
    GLuint           m_iRBO   = 0;
    CFramebuffer     m_sFramebuffer;
    CMonitor*        m_pMonitor = nullptr;
    wl_event_source* m_pFDWrite = nullptr;
};