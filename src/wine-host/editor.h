// Use the native version of xcb
#pragma push_macro("_WIN32")
#undef _WIN32
#include <xcb/xcb.h>
#pragma pop_macro("_WIN32")

#define NOMINMAX
#define NOSERVICE
#define NOMCX
#define NOIMM
#define WIN32_LEAN_AND_MEAN
#include <vestige/aeffectx.h>
#include <windows.h>

#include <memory>
#include <optional>
#include <string>

/**
 * A wrapper around the win32 windowing API to create and destroy editor
 * windows. A VST plugin can embed itself in that window, and we can then later
 * embed the window in a VST host provided X11 window.
 */
class Editor {
   public:
    /**
     * @param window_class_name The name for the window class for editor
     *   windows.
     */
    Editor(std::string window_class_name);

    /**
     * Open a window and return a handle to the new Win32 window that can be
     * used by the hosted VST plugin.
     */
    HWND open();
    void close();

    /**
     * Resize the window to match the given size, if open.
     *
     * @param new_size The rectangle with the plugin's current position.
     *
     * @return Whether the resizing was succesful. Will return false if the
     *   editor isn't open.
     */
    bool resize(const VstRect& new_size);

    /**
     * Pump messages from the editor GUI's event loop until all events are
     * process. Must be run from the same thread the GUI was created in because
     * of Win32 limitations. I guess that's what `effEditIdle` is for.
     */
    void handle_events();

    /**
     * Embed the (open) window into a parent window.
     *
     * @param parent_window_handle The X11 window handle passed by the VST host
     *   for the editor to embed itself into.
     *
     * @return Whether the embedding was succesful. Will return false if the
     *   window is not open.
     */
    bool embed_into(const size_t parent_window_handle);

   private:
    /**
     * Return the X11 window handle for the window if it's currently open.
     */
    std::optional<size_t> get_x11_handle();

    /**
     * The Win32 window class registered for the window window.
     */
    ATOM window_class;

    /**
     * A pointer to the currently active window. Will be a null pointer if no
     * window is active.
     */
    std::optional<
        std::unique_ptr<std::remove_pointer_t<HWND>, decltype(&DestroyWindow)>>
        win32_handle;

    std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)> x11_connection;
    xcb_atom_t xcb_xembed_info;
};
