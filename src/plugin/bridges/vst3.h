// yabridge: a Wine plugin bridge
// Copyright (C) 2020-2022 Robbert van der Helm
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <shared_mutex>
#include <thread>

#include "../../common/communication/vst3.h"
#include "../../common/logging/vst3.h"
#include "../../common/mutual-recursion.h"
#include "common.h"
#include "vst3-impls/plugin-factory-proxy.h"

// Forward declarations
class Vst3PluginProxyImpl;

/**
 * This handles the communication between the native host and a VST3 plugin
 * hosted in our Wine plugin host. VST3 is handled very differently from VST2
 * because a plugin is no longer its own entity, but rather a definition of
 * objects that the host can create and interconnect. This `Vst3PluginBridge`
 * will be instantiated when the plugin first gets loaded, and it will survive
 * until the last instance of the plugin gets removed. The Wine host process
 * will thus also have the same lifetime, and even with yabridge's 'individual'
 * plugin hosting other instances of the same plugin will be handled by a single
 * process.
 *
 * @remark See the comments at the top of `vst3-plugin.cpp` for more
 *   information.
 *
 * The naming scheme of all of these 'bridge' classes is `<type>{,Plugin}Bridge`
 * for greppability reasons. The `Plugin` infix is added on the native plugin
 * side.
 */
class Vst3PluginBridge : PluginBridge<Vst3Sockets<std::jthread>> {
   public:
    /**
     * Initializes the VST3 module by starting and setting up communicating with
     * the Wine plugin host.
     *
     * @param plugin_path The path to the **native** plugin library `.so` file.
     *   This is used to determine the path to the Windows plugin library we
     *   should load. For directly loaded bridges this should be
     *   `get_this_file_location()`. Chainloaded plugins should use the path of
     *   the chainloader copy instead.
     *
     * @throw std::runtime_error Thrown when the Wine plugin host could not be
     *   found, or if it could not locate and load a VST3 module.
     */
    explicit Vst3PluginBridge(const ghc::filesystem::path& plugin_path);

    /**
     * Terminate the Wine plugin host process and drop all work when the module
     * gets unloaded.
     */
    ~Vst3PluginBridge() noexcept;

    /**
     * When the host loads the module it will call `GetPluginFactory()` which
     * will in turn call this function. The idea is that we return an
     * `IPluginFactory*` that acts as an owned `IPtr<IPluginFactory>`, so we
     * have to manually increase the reference count here for every plugin
     * factory instance we return.
     *
     * @see plugin_factory_
     */
    Steinberg::IPluginFactory* get_plugin_factory();

    /**
     * Fetch the plugin proxy instance along with a lock valid for the
     * instance's lifetime. This is mostly just to save some boilerplate
     * everywhere. Use C++17's structured binding as syntactic sugar to not have
     * to deal with the lock handle.
     */
    std::pair<Vst3PluginProxyImpl&, std::shared_lock<std::shared_mutex>>
    get_proxy(size_t instance_id) noexcept;

    /**
     * Add a `Vst3PluginProxyImpl` to the list of registered proxy objects so we
     * can handle host callbacks. This function is called in
     * `Vst3PluginProxyImpl`'s constructor. If the plugin supports the
     * `IAudioProcessor` or `IComponent` interfaces, then we'll also connect to
     * a dedicated audio processing socket.
     *
     * @param proxy_object The proxy object so we can access its host context
     *   and unique instance identifier.
     *
     * @see plugin_proxies_
     */
    void register_plugin_proxy(Vst3PluginProxyImpl& proxy_object);

    /**
     * Remove a previously registered `Vst3PluginProxyImpl` from the list of
     * registered proxy objects. Called during the object's destructor after
     * asking the Wine plugin host to destroy the component on its side.
     *
     * @param proxy_object The proxy object so we can access its unique instance
     *   identifier.
     *
     * @see plugin_proxies_
     */
    void unregister_plugin_proxy(Vst3PluginProxyImpl& proxy_object);

    /**
     * Send a control message to the Wine plugin host return the response. This
     * is a shorthand for `sockets_.host_vst_control_.send_message()` for use in
     * VST3 interface implementations.
     */
    template <typename T>
    typename T::Response send_message(const T& object) {
        return sockets_.host_vst_control_.send_message(
            object, std::pair<Vst3Logger&, bool>(logger_, true));
    }

    /**
     * Send an `IAudioProcessor` or `IComponent` control message to a specific
     * plugin instance. This is separated from the above `send_message()` for
     * performance reasons, as this way every instance has its own dedicated
     * socket and thread.
     */
    template <typename T>
    typename T::Response send_audio_processor_message(const T& object) {
        return sockets_.send_audio_processor_message(
            object, std::pair<Vst3Logger&, bool>(logger_, true));
    }

    /**
     * Send an `IAudioProcessor` or `IComponent` control message to a specific
     * plugin instance, receiving the results into an existing object. This is
     * similar to the `send_audio_processor_message()` above, but this lets us
     * avoid allocations in response objects that contain heap data.
     */
    template <typename T>
    typename T::Response& receive_audio_processor_message_into(
        const T& object,
        typename T::Response& response_object) {
        return sockets_.receive_audio_processor_message_into(
            object, response_object,
            std::pair<Vst3Logger&, bool>(logger_, true));
    }

    /**
     * Send a message, and allow other threads to call functions on _this
     * thread_ while we're waiting for a response. This lets us execute
     * functions from the host's GUI thread while it is also calling functions
     * from that same thread. Because of that, we also know that while this
     * function is being called the host won't be able to handle any `IRunLoop`
     * events. We need this to support REAPER, because REAPER requires function
     * calls involving the GUI to be run from the GUI thread. Grep for
     * `run_gui_task` for instances of this.
     *
     * We use the same trick in `Vst3Bridge`.
     */
    template <typename T>
    typename T::Response send_mutually_recursive_message(const T& object) {
        return mutual_recursion_.fork([&]() { return send_message(object); });
    }

    /**
     * If `send_mutually_recursive_message()` is currently being called, then
     * run `fn` on the thread that's currently calling that function and return
     * the result of the call. If there's currently no mutually recursive
     * function call going on, this will return an `std::nullopt`, and the
     * caller should call `fn` itself.
     *
     * @return The result of calling `fn`, if `fn` was called.
     *
     * @see Vst3PlugViewProxyImpl::run_gui_task
     */
    template <std::invocable F>
    std::optional<std::invoke_result_t<F>> maybe_run_on_mutual_recursion_thread(
        F&& fn) {
        return mutual_recursion_.maybe_handle(std::forward<F>(fn));
    }

    /**
     * The logging facility used for this instance of yabridge. Wraps around
     * `PluginBridge::generic_logger`.
     */
    Vst3Logger logger_;

   private:
    /**
     * Handles callbacks from the plugin to the host over the
     * `vst_host_callback_` sockets.
     */
    std::jthread host_callback_handler_;

    /**
     * Our plugin factory. All information about the plugin and its supported
     * classes are copied directly from the Windows VST3 plugin's factory on the
     * Wine side, and we'll provide an implementation that can send control
     * messages to the Wine plugin host.
     *
     * @related get_plugin_factory
     */
    Steinberg::IPtr<Vst3PluginFactoryProxyImpl> plugin_factory_ = nullptr;

    /**
     * All VST3 plugin objects we created from this plugin. We keep track of
     * these in case the plugin does a host callback, so we can associate that
     * call with the exact host context object passed to it during a call to
     * `initialize()`. The IDs here are the same IDs as generated by the Wine
     * plugin host. An instance is added here through a call by
     * `register_plugin_proxy()` in `Vst3PluginProxyImpl`'s constructor, and an
     * instance is then removed through a call to `unregister_plugin_proxy()` in
     * the destructor.
     */
    std::unordered_map<size_t, std::reference_wrapper<Vst3PluginProxyImpl>>
        plugin_proxies_;

    /**
     * In theory all object handling is safe iff the host also doesn't do
     * anything weird even without locks, but we'll still prevent adding or
     * removing instances while accessing other instances at the same time
     * anyways. See `Vst3Bridge::object_instances_mutex` for more details.
     *
     * TODO: At some point replace this with a multiple reader single writer
     *       lock based by a spinlock. Because this lock is rarely contested
     *       `get_proxy()` never yields to the scheduler during audio
     *       processing, but it's still something we should avoid at all costs.
     */
    std::shared_mutex plugin_proxies_mutex_;

    /**
     * Used in `Vst3Bridge::send_mutually_recursive_message()` to be able to
     * execute functions from that same calling thread while we're waiting for a
     * response. This is used in `Vst3PlugViewProxyImpl::run_loop_tasks()`.
     */
    MutualRecursionHelper<std::jthread> mutual_recursion_;
};
