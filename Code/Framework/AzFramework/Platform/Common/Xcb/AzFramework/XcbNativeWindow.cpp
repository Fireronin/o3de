/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include <AzCore/Settings/SettingsRegistry.h>
#include <AzCore/std/containers/vector.h>
#include <AzFramework/Application/Application.h>
#include <AzFramework/Asset/AssetSystemBus.h>
#include <AzFramework/Windowing/NativeWindow.h>
#include <AzFramework/XcbConnectionManager.h>
#include <AzFramework/XcbInterface.h>
#include <AzFramework/XcbNativeWindow.h>

#include <png.h>
#include <xcb/xcb.h>
#include <xcb/xcb_image.h>

namespace AzFramework
{
    [[maybe_unused]] const char XcbErrorWindow[] = "XcbNativeWindow";
    static constexpr uint8_t s_XcbFormatDataSize = 32; // Format indicator for xcb for client messages
    static constexpr uint16_t s_DefaultXcbWindowBorderWidth = 4; // The default border with in pixels if a border was specified

#define _NET_WM_STATE_REMOVE 0l
#define _NET_WM_STATE_ADD 1l
#define _NET_WM_STATE_TOGGLE 2l

    ////////////////////////////////////////////////////////////////////////////////////////////////
    XcbNativeWindow::XcbNativeWindow()
        : NativeWindow::Implementation()
        , m_xcbConnection(nullptr)
        , m_xcbRootScreen(nullptr)
        , m_xcbWindow(XCB_NONE)
    {
        if (auto xcbConnectionManager = AzFramework::XcbConnectionManagerInterface::Get(); xcbConnectionManager != nullptr)
        {
            m_xcbConnection = xcbConnectionManager->GetXcbConnection();
        }
        AZ_Error(XcbErrorWindow, m_xcbConnection != nullptr, "Unable to get XCB Connection");
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////
    XcbNativeWindow::~XcbNativeWindow()
    {
        if (XCB_NONE != m_xcbWindow)
        {
            xcb_destroy_window(m_xcbConnection, m_xcbWindow);
        }
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////
    void XcbNativeWindow::InitWindow(const AZStd::string& title, const WindowGeometry& geometry, const WindowStyleMasks& styleMasks)
    {
        // Get the parent window
        const xcb_setup_t* xcbSetup = xcb_get_setup(m_xcbConnection);
        m_xcbRootScreen = xcb_setup_roots_iterator(xcbSetup).data;
        xcb_window_t xcbParentWindow = m_xcbRootScreen->root;

        m_xcbGraphicContext = xcb_generate_id(m_xcbConnection);
        uint32_t gc_mask = XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES;
        uint32_t gc_values[2] = { m_xcbRootScreen->black_pixel, 0 };

        xcb_create_gc(m_xcbConnection, m_xcbGraphicContext, xcbParentWindow, gc_mask, gc_values);

        // Create an XCB window from the connection
        m_xcbWindow = xcb_generate_id(m_xcbConnection);

        uint16_t borderWidth = 0;
        const uint32_t mask = styleMasks.m_platformAgnosticStyleMask;
        if ((mask & WindowStyleMasks::WINDOW_STYLE_BORDERED) || (mask & WindowStyleMasks::WINDOW_STYLE_RESIZEABLE))
        {
            borderWidth = s_DefaultXcbWindowBorderWidth;
        }

        uint32_t eventMask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;

        const uint32_t interestedEvents = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
            XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_EXPOSURE;

        uint32_t valueList[] = { m_xcbRootScreen->black_pixel, interestedEvents };

        xcb_void_cookie_t xcbCheckResult;

        xcbCheckResult = xcb_create_window_checked(
            m_xcbConnection,
            XCB_COPY_FROM_PARENT,
            m_xcbWindow,
            xcbParentWindow,
            aznumeric_cast<int16_t>(geometry.m_posX),
            aznumeric_cast<int16_t>(geometry.m_posY),
            aznumeric_cast<int16_t>(geometry.m_width),
            aznumeric_cast<int16_t>(geometry.m_height),
            borderWidth,
            XCB_WINDOW_CLASS_INPUT_OUTPUT,
            m_xcbRootScreen->root_visual,
            eventMask,
            valueList);

        AZ_Assert(ValidateXcbResult(xcbCheckResult), "Failed to create xcb window.");

        SetWindowTitle(title);

        m_posX = geometry.m_posX;
        m_posY = geometry.m_posY;
        m_width = geometry.m_width;
        m_height = geometry.m_height;

        InitializeAtoms();

        xcb_client_message_event_t event;
        event.response_type = XCB_CLIENT_MESSAGE;
        event.type = _NET_REQUEST_FRAME_EXTENTS;
        event.window = m_xcbWindow;
        event.format = 32;
        event.sequence = 0;
        event.data.data32[0] = 0l;
        event.data.data32[1] = 0l;
        event.data.data32[2] = 0l;
        event.data.data32[3] = 0l;
        event.data.data32[4] = 0l;
        xcbCheckResult = xcb_send_event(
            m_xcbConnection,
            1,
            m_xcbRootScreen->root,
            XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
            (const char*)&event);
        AZ_Assert(ValidateXcbResult(xcbCheckResult), "Failed to set _NET_REQUEST_FRAME_EXTENTS");

        // The WM will be able to kill the application if it gets unresponsive.
        int32_t pid = getpid();
        xcb_change_property(m_xcbConnection, XCB_PROP_MODE_REPLACE, m_xcbWindow, _NET_WM_PID, XCB_ATOM_CARDINAL, 32, 1, &pid);

        xcb_flush(m_xcbConnection);
    }

    xcb_atom_t XcbNativeWindow::GetAtom(const char* atomName)
    {
        xcb_intern_atom_cookie_t intern_atom_cookie = xcb_intern_atom(m_xcbConnection, 0, strlen(atomName), atomName);
        XcbStdFreePtr<xcb_intern_atom_reply_t> xkbinternAtom{ xcb_intern_atom_reply(m_xcbConnection, intern_atom_cookie, NULL) };

        if (!xkbinternAtom)
        {
            AZ_Error(XcbErrorWindow, xkbinternAtom != nullptr, "Unable to query xcb '%s' atom", atomName);
            return XCB_NONE;
        }

        return xkbinternAtom->atom;
    }

    int XcbNativeWindow::SetAtom(xcb_window_t window, xcb_atom_t atom, xcb_atom_t type, size_t len, void* data)
    {
        xcb_void_cookie_t cookie = xcb_change_property_checked(m_xcbConnection, XCB_PROP_MODE_REPLACE, window, atom, type, 32, len, data);
        XcbStdFreePtr<xcb_generic_error_t> xkbError{ xcb_request_check(m_xcbConnection, cookie) };

        if (!xkbError)
        {
            return 0;
        }

        return xkbError->error_code;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////

    void XcbNativeWindow::InitializeAtoms()
    {
        _NET_ACTIVE_WINDOW = GetAtom("_NET_ACTIVE_WINDOW");
        _NET_WM_BYPASS_COMPOSITOR = GetAtom("_NET_WM_BYPASS_COMPOSITOR");

        // ---------------------------------------------------------------------
        // Handle all WM Protocols atoms.
        //

        WM_PROTOCOLS = GetAtom("WM_PROTOCOLS");

        // This atom is used to close a window. Emitted when user clicks the close button.
        WM_DELETE_WINDOW = GetAtom("WM_DELETE_WINDOW");
        _NET_WM_PING = GetAtom("_NET_WM_PING");

        const AZStd::array atoms{ WM_DELETE_WINDOW, _NET_WM_PING };

        xcb_change_property(
            m_xcbConnection, XCB_PROP_MODE_REPLACE, m_xcbWindow, WM_PROTOCOLS, XCB_ATOM_ATOM, 32, atoms.size(), atoms.data());

        xcb_flush(m_xcbConnection);

        // ---------------------------------------------------------------------
        // Handle all WM State atoms.
        //

        _NET_WM_STATE = GetAtom("_NET_WM_STATE");
        _NET_WM_STATE_FULLSCREEN = GetAtom("_NET_WM_STATE_FULLSCREEN");
        _NET_WM_STATE_MAXIMIZED_VERT = GetAtom("_NET_WM_STATE_MAXIMIZED_VERT");
        _NET_WM_STATE_MAXIMIZED_HORZ = GetAtom("_NET_WM_STATE_MAXIMIZED_HORZ");
        _NET_MOVERESIZE_WINDOW = GetAtom("_NET_MOVERESIZE_WINDOW");
        _NET_REQUEST_FRAME_EXTENTS = GetAtom("_NET_REQUEST_FRAME_EXTENTS");
        _NET_FRAME_EXTENTS = GetAtom("_NET_FRAME_EXTENTS");
        _NET_WM_PID = GetAtom("_NET_WM_PID");
    }

    void XcbNativeWindow::GetWMStates()
    {
        xcb_get_property_cookie_t cookie = xcb_get_property(m_xcbConnection, 0, m_xcbWindow, _NET_WM_STATE, XCB_ATOM_ATOM, 0, 1024);

        xcb_generic_error_t* error = nullptr;
        XcbStdFreePtr<xcb_get_property_reply_t> xkbGetPropertyReply{ xcb_get_property_reply(m_xcbConnection, cookie, &error) };

        if (!xkbGetPropertyReply || error || !((xkbGetPropertyReply->format == 32) && (xkbGetPropertyReply->type == XCB_ATOM_ATOM)))
        {
            AZ_Warning("ApplicationLinux", false, "Acquiring _NET_WM_STATE information from the WM failed.");

            if (error)
            {
                AZ_TracePrintf("Error", "Error code %d", error->error_code);
                free(error);
            }
            return;
        }

        m_fullscreenState = false;
        m_horizontalyMaximized = false;
        m_verticallyMaximized = false;

        const xcb_atom_t* states = static_cast<const xcb_atom_t*>(xcb_get_property_value(xkbGetPropertyReply.get()));
        for (int i = 0; i < xkbGetPropertyReply->length; i++)
        {
            if (states[i] == _NET_WM_STATE_FULLSCREEN)
            {
                m_fullscreenState = true;
            }
            else if (states[i] == _NET_WM_STATE_MAXIMIZED_HORZ)
            {
                m_horizontalyMaximized = true;
            }
            else if (states[i] == _NET_WM_STATE_MAXIMIZED_VERT)
            {
                m_verticallyMaximized = true;
            }
        }
    }

    void XcbNativeWindow::DrawSplash()
    {
        auto settingsRegistry = AZ::SettingsRegistry::Get();
        AZ::SettingsRegistryInterface::FixedValueString splashScreenImagePath;
        AZ::SettingsRegistryInterface::FixedValueString assetCachePath;
        static const AZStd::string SplashLogoSetregPath = "/O3DE/xcb/SplashScreenImagePath";
        static const AZStd::string AssetsSetregPath = "/O3DE/Runtime/FilePaths/CacheProjectRootFolder";

        if (!(settingsRegistry && settingsRegistry->Get(splashScreenImagePath, SplashLogoSetregPath.c_str())))
        {
            printf("SplashScreenImagePath not found\n");
            return;
        }
        if (!(settingsRegistry && settingsRegistry->Get(assetCachePath, AssetsSetregPath.c_str())))
        {
            printf("Failed to grab cache folder\n");
            return;
        }
        const AZStd::string splashScreenImagePath2 =
            AZStd::string::format("%s/linux/%s", assetCachePath.c_str(), splashScreenImagePath.c_str());

        FILE* fp = fopen(splashScreenImagePath2.c_str(), "rb");
        if (!fp)
        {
            printf("Failed to open image %s \n", splashScreenImagePath2.c_str());
            return;
        }
        // Initialize libpng
        png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
        png_infop info_ptr = png_create_info_struct(png_ptr);
        png_init_io(png_ptr, fp);
        png_read_info(png_ptr, info_ptr);

        // Read the PNG into an in-memory format that XCB can use
        int width = png_get_image_width(png_ptr, info_ptr);
        int height = png_get_image_height(png_ptr, info_ptr);

        png_bytep row_pointers[height];
        for (int y = 0; y < height; y++)
        {
            row_pointers[y] = (png_byte*)malloc(png_get_rowbytes(png_ptr, info_ptr));
        }
        png_read_image(png_ptr, row_pointers);
        fclose(fp);
        unsigned int stride = width * 4;
        uint8_t* data = (uint8_t*)malloc(height * stride);
        for (int y = 0; y < height; y++)
        {
            for (int x = 0; x < width; x++)
            {
                png_bytep px = &(row_pointers[y][x * 4]);
                int pixelIndex = (x + y * width) * 4; // 3 bytes per pixel (RGB)
                data[pixelIndex + 0] = px[2]; // R
                data[pixelIndex + 1] = px[1]; // G
                data[pixelIndex + 2] = px[0]; // B
            }
            free(row_pointers[y]);
        }

        xcb_void_cookie_t cookie;
        xcb_pixmap_t pixmap = xcb_generate_id(m_xcbConnection);
        cookie = xcb_create_pixmap_checked(m_xcbConnection, m_xcbRootScreen->root_depth, pixmap, m_xcbWindow, width, height);
        if (xcb_generic_error_t* error = xcb_request_check(m_xcbConnection, cookie))
        {
            printf("Error in xcb_create_pixmap: %d\n", error->error_code);
            free(error);
            return;
        }
        // This uses hardcoded 8bit depth , TODO check on HDR monitor
        xcb_image_t* image =
            xcb_image_create_native(m_xcbConnection, width, height, XCB_IMAGE_FORMAT_Z_PIXMAP, 24, data, width * height * 4, data);
        xcb_image_put(m_xcbConnection, pixmap, m_xcbGraphicContext, image, 0, 0, 0);
        xcb_image_destroy(image); // not needed free(data);  Freed by destroy image
        xcb_flush(m_xcbConnection);

        xcb_generic_event_t* event;
        while (true)
        {
            event = xcb_wait_for_event(m_xcbConnection);
            switch (event->response_type & ~0x80)
            {
            case XCB_EXPOSE:
                {
                    xcb_expose_event_t* x = (xcb_expose_event_t*)event;
                    xcb_copy_area(
                        m_xcbConnection,
                        pixmap,
                        m_xcbWindow,
                        m_xcbGraphicContext,
                        x->x,
                        x->y,
                        (m_width - width) / 2,
                        (m_height - height) / 2,
                        x->width,
                        x->height);
                    xcb_flush(m_xcbConnection);
                    goto end;
                }
                break;
            default:
                break;
            }
            free(event);
        }

    end:

        xcb_free_pixmap(m_xcbConnection, pixmap);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////
    void XcbNativeWindow::Activate()
    {
        XcbEventHandlerBus::Handler::BusConnect();

        if (!m_activated) // nothing to do if window was already activated
        {
            xcb_map_window(m_xcbConnection, m_xcbWindow);
            xcb_flush(m_xcbConnection);
            DrawSplash();
            m_activated = true;
        }
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////
    void XcbNativeWindow::Deactivate()
    {
        if (m_activated) // nothing to do if window was already deactivated
        {
            m_activated = false;

            WindowNotificationBus::Event(reinterpret_cast<NativeWindowHandle>(m_xcbWindow), &WindowNotificationBus::Events::OnWindowClosed);

            xcb_unmap_window(m_xcbConnection, m_xcbWindow);
            xcb_flush(m_xcbConnection);
        }
        XcbEventHandlerBus::Handler::BusDisconnect();
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////
    NativeWindowHandle XcbNativeWindow::GetWindowHandle() const
    {
        return reinterpret_cast<NativeWindowHandle>(m_xcbWindow);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////
    void XcbNativeWindow::SetWindowTitle(const AZStd::string& title)
    {
        // Set the title of both the window and the task bar by using
        // a buffer to hold the title twice, separated by a null-terminator
        auto doubleTitleSize = (title.size() + 1) * 2;
        AZStd::string doubleTitle(doubleTitleSize, '\0');
        azstrncpy(doubleTitle.data(), doubleTitleSize, title.c_str(), title.size());
        azstrncpy(&doubleTitle.data()[title.size() + 1], title.size(), title.c_str(), title.size());

        xcb_void_cookie_t xcbCheckResult;
        xcbCheckResult = xcb_change_property(
            m_xcbConnection,
            XCB_PROP_MODE_REPLACE,
            m_xcbWindow,
            XCB_ATOM_WM_CLASS,
            XCB_ATOM_STRING,
            8,
            static_cast<uint32_t>(doubleTitle.size()),
            doubleTitle.c_str());
        AZ_Assert(ValidateXcbResult(xcbCheckResult), "Failed to set window title.");
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////
    void XcbNativeWindow::ResizeClientArea(WindowSize clientAreaSize, const WindowPosOptions& options)
    {
        const uint32_t values[] = { clientAreaSize.m_width, clientAreaSize.m_height };

        if (m_activated)
        {
            xcb_unmap_window(m_xcbConnection, m_xcbWindow);
        }
        xcb_configure_window(m_xcbConnection, m_xcbWindow, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);

        if (m_activated)
        {
            xcb_map_window(m_xcbConnection, m_xcbWindow);
            xcb_flush(m_xcbConnection);
        }
        // Notify the RHI to rebuild swapchain and swapchain images after updating the surface
        WindowSizeChanged(clientAreaSize.m_width, clientAreaSize.m_height);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////
    bool XcbNativeWindow::SupportsClientAreaResize() const
    {
        return true;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////
    uint32_t XcbNativeWindow::GetDisplayRefreshRate() const
    {
        // [GFX TODO][GHI - 2678]
        // Using 60 for now until proper support is added

        return 60;
    }

    bool XcbNativeWindow::GetFullScreenState() const
    {
        return m_fullscreenState;
    }

    void XcbNativeWindow::SetFullScreenState(bool fullScreenState)
    {
        // TODO This is a pretty basic full-screen implementation using WM's _NET_WM_STATE_FULLSCREEN state.
        // Do we have to provide also the old way?

        GetWMStates();

        xcb_client_message_event_t event;
        event.response_type = XCB_CLIENT_MESSAGE;
        event.type = _NET_WM_STATE;
        event.window = m_xcbWindow;
        event.format = 32;
        event.sequence = 0;
        event.data.data32[0] = fullScreenState ? _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE;
        event.data.data32[1] = _NET_WM_STATE_FULLSCREEN;
        event.data.data32[2] = 0;
        event.data.data32[3] = 1;
        event.data.data32[4] = 0;
        [[maybe_unused]] xcb_void_cookie_t xcbCheckResult = xcb_send_event(
            m_xcbConnection,
            1,
            m_xcbRootScreen->root,
            XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
            (const char*)&event);
        AZ_Assert(ValidateXcbResult(xcbCheckResult), "Failed to set _NET_WM_STATE_FULLSCREEN");

        // Also try to disable/enable the compositor if possible. Might help in some cases.
        const long _NET_WM_BYPASS_COMPOSITOR_HINT_ON = m_fullscreenState ? 1 : 0;
        SetAtom(m_xcbWindow, _NET_WM_BYPASS_COMPOSITOR, XCB_ATOM_CARDINAL, 32, (char*)&_NET_WM_BYPASS_COMPOSITOR_HINT_ON);

        if (!fullScreenState)
        {
            if (m_horizontalyMaximized || m_verticallyMaximized)
            {
                printf("Remove maximized state.\n");
                xcb_client_message_event_t event;
                event.response_type = XCB_CLIENT_MESSAGE;
                event.type = _NET_WM_STATE;
                event.window = m_xcbWindow;
                event.format = 32;
                event.sequence = 0;
                event.data.data32[0] = _NET_WM_STATE_MAXIMIZED_VERT;
                event.data.data32[1] = _NET_WM_STATE_MAXIMIZED_HORZ;
                event.data.data32[2] = 0;
                event.data.data32[3] = 0;
                event.data.data32[4] = 0;
                [[maybe_unused]] xcb_void_cookie_t xcbCheckResult = xcb_send_event(
                    m_xcbConnection,
                    1,
                    m_xcbRootScreen->root,
                    XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                    (const char*)&event);
                AZ_Assert(
                    ValidateXcbResult(xcbCheckResult), "Failed to remove _NET_WM_STATE_MAXIMIZED_VERT | _NET_WM_STATE_MAXIMIZED_HORZ");
            }
        }

        xcb_flush(m_xcbConnection);
        m_fullscreenState = fullScreenState;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////
    bool XcbNativeWindow::ValidateXcbResult(xcb_void_cookie_t cookie)
    {
        bool result = true;
        if (xcb_generic_error_t* error = xcb_request_check(m_xcbConnection, cookie))
        {
            AZ_TracePrintf("Error", "Error code %d", error->error_code);
            result = false;
        }
        return result;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////
    void XcbNativeWindow::HandleXcbEvent(xcb_generic_event_t* event)
    {
        switch (event->response_type & s_XcbResponseTypeMask)
        {
        case XCB_CONFIGURE_NOTIFY:
            {
                xcb_configure_notify_event_t* cne = reinterpret_cast<xcb_configure_notify_event_t*>(event);
                if ((cne->width != m_width) || (cne->height != m_height))
                {
                    WindowSizeChanged(aznumeric_cast<uint32_t>(cne->width), aznumeric_cast<uint32_t>(cne->height));
                }
                break;
            }
        case XCB_CLIENT_MESSAGE:
            {
                xcb_client_message_event_t* cme = reinterpret_cast<xcb_client_message_event_t*>(event);

                if ((cme->type == WM_PROTOCOLS) && (cme->format == s_XcbFormatDataSize))
                {
                    const xcb_atom_t protocolAtom = cme->data.data32[0];
                    if (protocolAtom == WM_DELETE_WINDOW)
                    {
                        Deactivate();

                        ApplicationRequests::Bus::Broadcast(&ApplicationRequests::ExitMainLoop);
                    }
                    else if (protocolAtom == _NET_WM_PING && cme->window != m_xcbRootScreen->root)
                    {
                        xcb_client_message_event_t reply = *cme;
                        reply.response_type = XCB_CLIENT_MESSAGE;
                        reply.window = m_xcbRootScreen->root;

                        xcb_send_event(
                            m_xcbConnection,
                            0,
                            m_xcbRootScreen->root,
                            XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                            reinterpret_cast<const char*>(&reply));
                        xcb_flush(m_xcbConnection);
                    }
                }
                break;
            }
        }
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////
    void XcbNativeWindow::WindowSizeChanged(const uint32_t width, const uint32_t height)
    {
        if (m_width != width || m_height != height)
        {
            m_width = width;
            m_height = height;

            if (m_activated)
            {
                WindowNotificationBus::Event(
                    reinterpret_cast<NativeWindowHandle>(m_xcbWindow), &WindowNotificationBus::Events::OnWindowResized, width, height);
                if (!m_enableCustomizedResolution)
                {
                    WindowNotificationBus::Event(GetWindowHandle(), &WindowNotificationBus::Events::OnResolutionChanged, width, height);
                }
            }
        }
    }
} // namespace AzFramework
