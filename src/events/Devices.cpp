#include "Events.hpp"

#include "../Compositor.hpp"
#include "../helpers/WLClasses.hpp"
#include "../managers/input/InputManager.hpp"
#include "../render/Renderer.hpp"
#include "../protocols/PointerGestures.hpp"

// ---------------------------------------------------- //
//   _____  ________      _______ _____ ______  _____   //
//  |  __ \|  ____\ \    / /_   _/ ____|  ____|/ ____|  //
//  | |  | | |__   \ \  / /  | || |    | |__  | (___    //
//  | |  | |  __|   \ \/ /   | || |    |  __|  \___ \   //
//  | |__| | |____   \  /   _| || |____| |____ ____) |  //
//  |_____/|______|   \/   |_____\_____|______|_____/   //
//                                                      //
// ---------------------------------------------------- //

void Events::listener_keyboardDestroy(void* owner, void* data) {
    SKeyboard* PKEYBOARD = (SKeyboard*)owner;
    g_pInputManager->destroyKeyboard(PKEYBOARD);

    Debug::log(LOG, "Destroyed keyboard {:x}", (uintptr_t)PKEYBOARD);
}

void Events::listener_keyboardKey(void* owner, void* data) {
    SKeyboard* PKEYBOARD = (SKeyboard*)owner;
    g_pInputManager->onKeyboardKey((wlr_keyboard_key_event*)data, PKEYBOARD);
}

void Events::listener_keyboardMod(void* owner, void* data) {
    SKeyboard* PKEYBOARD = (SKeyboard*)owner;
    g_pInputManager->onKeyboardMod(data, PKEYBOARD);
}

void Events::listener_mouseFrame(wl_listener* listener, void* data) {
    wlr_seat_pointer_notify_frame(g_pCompositor->m_sSeat.seat);
}

void Events::listener_mouseMove(wl_listener* listener, void* data) {
    g_pInputManager->onMouseMoved((wlr_pointer_motion_event*)data);
}

void Events::listener_mouseMoveAbsolute(wl_listener* listener, void* data) {
    g_pInputManager->onMouseWarp((wlr_pointer_motion_absolute_event*)data);
}

void Events::listener_mouseButton(wl_listener* listener, void* data) {
    g_pInputManager->onMouseButton((wlr_pointer_button_event*)data);
}

void Events::listener_mouseAxis(wl_listener* listener, void* data) {
    g_pInputManager->onMouseWheel((wlr_pointer_axis_event*)data);
}

void Events::listener_requestMouse(wl_listener* listener, void* data) {
    const auto EVENT = (wlr_seat_pointer_request_set_cursor_event*)data;

    g_pInputManager->processMouseRequest(EVENT);
}

void Events::listener_newInput(wl_listener* listener, void* data) {
    const auto DEVICE = (wlr_input_device*)data;

    switch (DEVICE->type) {
        case WLR_INPUT_DEVICE_KEYBOARD:
            Debug::log(LOG, "Attached a keyboard with name {}", DEVICE->name);
            g_pInputManager->newKeyboard(DEVICE);
            break;
        case WLR_INPUT_DEVICE_POINTER:
            Debug::log(LOG, "Attached a mouse with name {}", DEVICE->name);
            g_pInputManager->newMouse(DEVICE);
            break;
        case WLR_INPUT_DEVICE_TOUCH:
            Debug::log(LOG, "Attached a touch device with name {}", DEVICE->name);
            g_pInputManager->newTouchDevice(DEVICE);
            break;
        case WLR_INPUT_DEVICE_TABLET:
            Debug::log(LOG, "Attached a tablet tool with name {}", DEVICE->name);
            g_pInputManager->newTabletTool(DEVICE);
            break;
        case WLR_INPUT_DEVICE_TABLET_PAD:
            Debug::log(LOG, "Attached a tablet pad with name {}", DEVICE->name);
            g_pInputManager->newTabletPad(DEVICE);
            break;
        case WLR_INPUT_DEVICE_SWITCH:
            Debug::log(LOG, "Attached a switch device with name {}", DEVICE->name);
            g_pInputManager->newSwitch(DEVICE);
            break;
        default: Debug::log(WARN, "Unrecognized input device plugged in: {}", DEVICE->name); break;
    }

    g_pInputManager->updateCapabilities();
}

void Events::listener_destroyMouse(void* owner, void* data) {
    const auto PMOUSE = (SMouse*)owner;

    g_pInputManager->destroyMouse(PMOUSE->mouse);
}

void Events::listener_swipeBegin(wl_listener* listener, void* data) {
    const auto EVENT = (wlr_pointer_swipe_begin_event*)data;

    g_pInputManager->onSwipeBegin(EVENT);
}

void Events::listener_swipeUpdate(wl_listener* listener, void* data) {
    const auto EVENT = (wlr_pointer_swipe_update_event*)data;

    g_pInputManager->onSwipeUpdate(EVENT);
}

void Events::listener_swipeEnd(wl_listener* listener, void* data) {
    const auto EVENT = (wlr_pointer_swipe_end_event*)data;

    g_pInputManager->onSwipeEnd(EVENT);
}

void Events::listener_pinchBegin(wl_listener* listener, void* data) {
    const auto EV = (wlr_pointer_pinch_begin_event*)data;
    PROTO::pointerGestures->pinchBegin(EV->time_msec, EV->fingers);
}

void Events::listener_pinchUpdate(wl_listener* listener, void* data) {
    const auto EV = (wlr_pointer_pinch_update_event*)data;
    PROTO::pointerGestures->pinchUpdate(EV->time_msec, {EV->dx, EV->dy}, EV->scale, EV->rotation);
}

void Events::listener_pinchEnd(wl_listener* listener, void* data) {
    const auto EV = (wlr_pointer_pinch_end_event*)data;
    PROTO::pointerGestures->pinchEnd(EV->time_msec, EV->cancelled);
}

void Events::listener_touchBegin(wl_listener* listener, void* data) {
    g_pInputManager->onTouchDown((wlr_touch_down_event*)data);
}

void Events::listener_touchEnd(wl_listener* listener, void* data) {
    g_pInputManager->onTouchUp((wlr_touch_up_event*)data);
}

void Events::listener_touchUpdate(wl_listener* listener, void* data) {
    g_pInputManager->onTouchMove((wlr_touch_motion_event*)data);
}

void Events::listener_touchFrame(wl_listener* listener, void* data) {
    wlr_seat_touch_notify_frame(g_pCompositor->m_sSeat.seat);
}

void Events::listener_holdBegin(wl_listener* listener, void* data) {
    const auto EV = (wlr_pointer_hold_begin_event*)data;
    PROTO::pointerGestures->holdBegin(EV->time_msec, EV->fingers);
}

void Events::listener_holdEnd(wl_listener* listener, void* data) {
    const auto EV = (wlr_pointer_hold_end_event*)data;
    PROTO::pointerGestures->holdEnd(EV->time_msec, EV->cancelled);
}