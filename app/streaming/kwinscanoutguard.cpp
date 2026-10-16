#include "kwinscanoutguard.h"

#include <QDebug>
#ifdef HAVE_QTDBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#endif

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iterator>
#include <vector>

#include <dirent.h>
#include <dlfcn.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

// AppImage builds don't link libwayland-client; the host's copy must be used,
// so it is loaded at runtime like SDL does. The entry points used below are
// redirected to pointers resolved by loadWaylandClient(). The generated
// protocol headers must be included after these macros.
#include <wayland-client-core.h>

namespace {
decltype(&::wl_display_connect) dyn_wl_display_connect;
decltype(&::wl_display_connect_to_fd) dyn_wl_display_connect_to_fd;
decltype(&::wl_display_disconnect) dyn_wl_display_disconnect;
decltype(&::wl_display_dispatch) dyn_wl_display_dispatch;
decltype(&::wl_display_dispatch_pending) dyn_wl_display_dispatch_pending;
decltype(&::wl_display_flush) dyn_wl_display_flush;
decltype(&::wl_display_get_fd) dyn_wl_display_get_fd;
decltype(&::wl_display_prepare_read) dyn_wl_display_prepare_read;
decltype(&::wl_display_cancel_read) dyn_wl_display_cancel_read;
decltype(&::wl_display_read_events) dyn_wl_display_read_events;
decltype(&::wl_proxy_marshal_flags) dyn_wl_proxy_marshal_flags;
decltype(&::wl_proxy_add_listener) dyn_wl_proxy_add_listener;
decltype(&::wl_proxy_get_version) dyn_wl_proxy_get_version;
decltype(&::wl_proxy_destroy) dyn_wl_proxy_destroy;
}
extern "C" {
// wayland-client-protocol.h redeclares these through the macros below.
const struct wl_interface* moonlight_kwin_guard_wl_registry_interface;
const struct wl_interface* moonlight_kwin_guard_wl_callback_interface;
}

#define wl_display_connect (*dyn_wl_display_connect)
#define wl_display_connect_to_fd (*dyn_wl_display_connect_to_fd)
#define wl_display_disconnect (*dyn_wl_display_disconnect)
#define wl_display_dispatch (*dyn_wl_display_dispatch)
#define wl_display_dispatch_pending (*dyn_wl_display_dispatch_pending)
#define wl_display_flush (*dyn_wl_display_flush)
#define wl_display_get_fd (*dyn_wl_display_get_fd)
#define wl_display_prepare_read (*dyn_wl_display_prepare_read)
#define wl_display_cancel_read (*dyn_wl_display_cancel_read)
#define wl_display_read_events (*dyn_wl_display_read_events)
#define wl_proxy_marshal_flags (*dyn_wl_proxy_marshal_flags)
#define wl_proxy_add_listener (*dyn_wl_proxy_add_listener)
#define wl_proxy_get_version (*dyn_wl_proxy_get_version)
#define wl_proxy_destroy (*dyn_wl_proxy_destroy)
#define wl_registry_interface (*moonlight_kwin_guard_wl_registry_interface)
#define wl_callback_interface (*moonlight_kwin_guard_wl_callback_interface)

// The redeclarations of the interfaces through the macros are
// "extern const struct wl_interface (*name);".
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wparentheses"
#include <wayland-client-protocol.h>
#pragma GCC diagnostic pop
#include "streaming/video/ffmpeg-renderers/protocols/kde-output-device-v2-client-protocol.h"
#include "streaming/video/ffmpeg-renderers/protocols/kde-output-management-v2-client-protocol.h"

using namespace std::chrono_literals;

namespace {

constexpr auto k_PollInterval = 2s;
constexpr auto k_HdrCheckInterval = 10s;
constexpr auto k_HdrSettleTime = 1s;
// A busy KWin can miss a D-Bus deadline; only a lasting failure ends the
// effect check.
constexpr int k_MaxEffectQueryFailures = 3;
// Bounds every compositor wait, so stopping the guard never hangs the end of
// a stream.
constexpr auto k_WaylandTimeout = 2s;

bool loadWaylandClient()
{
    static std::once_flag once;
    static bool loaded = false;
    std::call_once(once, [] {
        void* library = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!library) {
            return;
        }
#define LOAD_WAYLAND_SYMBOL(name) \
        dyn_##name = reinterpret_cast<decltype(dyn_##name)>(dlsym(library, #name)); \
        if (!dyn_##name) return;
        LOAD_WAYLAND_SYMBOL(wl_display_connect)
        LOAD_WAYLAND_SYMBOL(wl_display_connect_to_fd)
        LOAD_WAYLAND_SYMBOL(wl_display_disconnect)
        LOAD_WAYLAND_SYMBOL(wl_display_dispatch)
        LOAD_WAYLAND_SYMBOL(wl_display_dispatch_pending)
        LOAD_WAYLAND_SYMBOL(wl_display_flush)
        LOAD_WAYLAND_SYMBOL(wl_display_get_fd)
        LOAD_WAYLAND_SYMBOL(wl_display_prepare_read)
        LOAD_WAYLAND_SYMBOL(wl_display_cancel_read)
        LOAD_WAYLAND_SYMBOL(wl_display_read_events)
        LOAD_WAYLAND_SYMBOL(wl_proxy_marshal_flags)
        LOAD_WAYLAND_SYMBOL(wl_proxy_add_listener)
        LOAD_WAYLAND_SYMBOL(wl_proxy_get_version)
        LOAD_WAYLAND_SYMBOL(wl_proxy_destroy)
#undef LOAD_WAYLAND_SYMBOL
        moonlight_kwin_guard_wl_registry_interface =
            static_cast<const struct wl_interface*>(dlsym(library, "wl_registry_interface"));
        moonlight_kwin_guard_wl_callback_interface =
            static_cast<const struct wl_interface*>(dlsym(library, "wl_callback_interface"));
        loaded = moonlight_kwin_guard_wl_registry_interface && moonlight_kwin_guard_wl_callback_interface;
    });
    return loaded;
}

// wl_display_roundtrip() with a deadline.
bool roundtrip(wl_display* display, std::chrono::steady_clock::time_point deadline)
{
    bool done = false;
    static const wl_callback_listener listener{
        [](void* data, wl_callback*, uint32_t) { *static_cast<bool*>(data) = true; }
    };
    wl_callback* callback = wl_display_sync(display);
    wl_callback_add_listener(callback, &listener, &done);
    while (!done && std::chrono::steady_clock::now() < deadline) {
        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) < 0 || done || std::chrono::steady_clock::now() >= deadline) {
                goto finished;
            }
        }
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display);
            break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            wl_display_cancel_read(display);
            break;
        }
        pollfd fd{wl_display_get_fd(display), POLLIN, 0};
        const int ready = poll(&fd, 1, int(remaining.count()));
        if (ready < 0 && errno == EINTR) {
            wl_display_cancel_read(display);
            continue;
        }
        if (ready <= 0 || (fd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            wl_display_cancel_read(display);
            break;
        }
        if (wl_display_read_events(display) < 0 || wl_display_dispatch_pending(display) < 0) break;
    }
finished:
    wl_callback_destroy(callback);
    return done;
}

struct OutputDevice
{
    kde_output_device_v2* proxy = nullptr;
    std::vector<kde_output_device_mode_v2*> modes;
    QString name;
    bool enabled = false;
    bool hdr = false;
    uint32_t sdrBrightness = 0;
    bool prefersAccuracy = false;
    bool complete = false;
    bool removed = false;
    bool fromRegistry = false;
    uint32_t globalName = 0;
    uint64_t revision = 0;
};

const kde_output_device_v2_listener* outputDeviceListener()
{
    static const kde_output_device_v2_listener listener = [] {
        constexpr auto ignore = [](void* data, auto...) {
            static_cast<OutputDevice*>(data)->complete = false;
        };
        kde_output_device_v2_listener l{};
        l.geometry = ignore;
        l.current_mode = ignore;
        l.mode = [](void* data, kde_output_device_v2*, kde_output_device_mode_v2* mode) {
            auto* output = static_cast<OutputDevice*>(data);
            output->complete = false;
            output->modes.push_back(mode);
            static const kde_output_device_mode_v2_listener modeListener{
                [](void*, kde_output_device_mode_v2*, int32_t, int32_t) {},
                [](void*, kde_output_device_mode_v2*, int32_t) {},
                [](void*, kde_output_device_mode_v2*) {},
                [](void* data, kde_output_device_mode_v2* mode) {
                    auto* output = static_cast<OutputDevice*>(data);
                    output->complete = false;
                    auto& modes = output->modes;
                    modes.erase(std::remove(modes.begin(), modes.end(), mode), modes.end());
                    kde_output_device_mode_v2_destroy(mode);
                },
                [](void*, kde_output_device_mode_v2*, uint32_t) {},
            };
            kde_output_device_mode_v2_add_listener(mode, &modeListener, output);
        };
        l.done = [](void* data, kde_output_device_v2*) {
            auto* output = static_cast<OutputDevice*>(data);
            output->complete = true;
            output->revision++;
        };
        l.scale = ignore;
        l.edid = ignore;
        l.enabled = [](void* data, kde_output_device_v2*, int32_t enabled) {
            auto* output = static_cast<OutputDevice*>(data);
            output->complete = false;
            output->enabled = enabled != 0;
        };
        l.uuid = ignore;
        l.serial_number = ignore;
        l.eisa_id = ignore;
        l.capabilities = ignore;
        l.overscan = ignore;
        l.vrr_policy = ignore;
        l.rgb_range = ignore;
        l.name = [](void* data, kde_output_device_v2*, const char* name) {
            auto* output = static_cast<OutputDevice*>(data);
            output->complete = false;
            output->name = QString::fromUtf8(name);
        };
        l.high_dynamic_range = [](void* data, kde_output_device_v2*, uint32_t enabled) {
            auto* output = static_cast<OutputDevice*>(data);
            output->complete = false;
            output->hdr = enabled != 0;
        };
        l.sdr_brightness = [](void* data, kde_output_device_v2*, uint32_t brightness) {
            auto* output = static_cast<OutputDevice*>(data);
            output->complete = false;
            output->sdrBrightness = brightness;
        };
        l.wide_color_gamut = ignore;
        l.auto_rotate_policy = ignore;
        l.icc_profile_path = ignore;
        l.brightness_metadata = ignore;
        l.brightness_overrides = ignore;
        l.sdr_gamut_wideness = ignore;
        l.color_profile_source = ignore;
        l.brightness = ignore;
        l.color_power_tradeoff = [](void* data, kde_output_device_v2*, uint32_t preference) {
            auto* output = static_cast<OutputDevice*>(data);
            output->complete = false;
            output->prefersAccuracy =
                preference == KDE_OUTPUT_DEVICE_V2_COLOR_POWER_TRADEOFF_ACCURACY;
        };
        l.dimming = ignore;
        l.replication_source = ignore;
        l.ddc_ci_allowed = ignore;
        l.max_bits_per_color = ignore;
        l.max_bits_per_color_range = ignore;
        l.automatic_max_bits_per_color_limit = ignore;
        l.edr_policy = ignore;
        l.sharpness = ignore;
        l.priority = ignore;
        l.auto_brightness = ignore;
        l.removed = [](void* data, kde_output_device_v2*) {
            auto* output = static_cast<OutputDevice*>(data);
            output->removed = true;
            output->enabled = false;
            output->revision++;
        };
        l.hdr_icc_profile_path = ignore;
        l.hdr_color_profile_source = ignore;
        l.abm_level = ignore;
        return l;
    }();
    return &listener;
}

// A short-lived connection to KWin's output management, with every output
// device bound and its current state received.
class OutputManagement
{
public:
    explicit OutputManagement(int connectedFd)
    {
        if (!loadWaylandClient()) {
            if (connectedFd >= 0) close(connectedFd);
            m_Error = QStringLiteral("libwayland-client.so.0 is unavailable");
            return;
        }
        m_Display = connectedFd >= 0 ? wl_display_connect_to_fd(connectedFd) : wl_display_connect(nullptr);
        if (!m_Display) {
            m_Error = QStringLiteral("cannot connect to the Wayland display");
            return;
        }
        static const wl_registry_listener registryListener{
            [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
                auto* self = static_cast<OutputManagement*>(data);
                // set_sdr_brightness needs version 4 of the manager and the
                // HDR state events version 3 of the device.
                if (!self->m_Manager && version >= 4 && !strcmp(interface, kde_output_management_v2_interface.name)) {
                    self->m_Manager = static_cast<kde_output_management_v2*>(wl_registry_bind(
                        registry, name, &kde_output_management_v2_interface,
                        std::min<uint32_t>(version, kde_output_management_v2_interface.version)));
                    self->m_ManagerGlobal = name;
                }
                else if (!self->m_DeviceRegistry && version >= 21 &&
                         !strcmp(interface, kde_output_device_registry_v2_interface.name)) {
                    // New KWin versions enumerate output objects here instead
                    // of publishing a global for every output. Version 21 is
                    // the first supported registry version; its new_id outputs
                    // inherit that version, so handle all events through 21.
                    self->m_DeviceRegistry = static_cast<kde_output_device_registry_v2*>(wl_registry_bind(
                        registry, name, &kde_output_device_registry_v2_interface, 21));
                    static const kde_output_device_registry_v2_listener outputRegistryListener{
                        [](void*, kde_output_device_registry_v2*) {},
                        [](void* data, kde_output_device_registry_v2*, kde_output_device_v2* output) {
                            static_cast<OutputManagement*>(data)->addDevice(output, true, 0);
                        },
                    };
                    kde_output_device_registry_v2_add_listener(self->m_DeviceRegistry, &outputRegistryListener, self);
                }
                else if (version >= 3 && !strcmp(interface, kde_output_device_v2_interface.name)) {
                    self->addDevice(static_cast<kde_output_device_v2*>(wl_registry_bind(
                        registry, name, &kde_output_device_v2_interface,
                        std::min<uint32_t>(version, 16))), false, name);
                }
            },
            [](void* data, wl_registry*, uint32_t name) {
                auto* self = static_cast<OutputManagement*>(data);
                if (name == self->m_ManagerGlobal) {
                    self->m_ManagerRemoved = true;
                    self->m_Error = QStringLiteral("KWin output management was removed");
                }
                for (const auto& output : self->m_Devices) {
                    if (output->globalName == name) {
                        output->removed = true;
                        output->enabled = false;
                        output->revision++;
                    }
                }
            }
        };
        m_Registry = wl_display_get_registry(m_Display);
        wl_registry_add_listener(m_Registry, &registryListener, this);
        // The first roundtrip binds the globals, the second receives the
        // devices' initial state.
        if (!roundtrip(m_Display, m_Deadline) || !roundtrip(m_Display, m_Deadline)) {
            m_Error = QStringLiteral("no answer from the Wayland compositor");
        }
        else if (!m_Manager) {
            m_Error = QStringLiteral("the compositor has no kde_output_management_v2 (not KWin)");
        }
    }

    ~OutputManagement()
    {
        for (const auto& device : m_Devices) {
            for (auto* mode : device->modes) {
                kde_output_device_mode_v2_destroy(mode);
            }
            if (kde_output_device_v2_get_version(device->proxy) >= 21) {
                kde_output_device_v2_release(device->proxy);
            }
            else {
                kde_output_device_v2_destroy(device->proxy);
            }
        }
        if (m_DeviceRegistry) kde_output_device_registry_v2_destroy(m_DeviceRegistry);
        if (m_Manager) kde_output_management_v2_destroy(m_Manager);
        if (m_Registry) wl_registry_destroy(m_Registry);
        if (m_Display) wl_display_disconnect(m_Display);
    }

    bool ok() const { return m_Error.isEmpty() && !m_ManagerRemoved; }
    const QString& error() const { return m_Error; }
    const std::vector<std::unique_ptr<OutputDevice>>& devices() const { return m_Devices; }
    bool usesRegistry() const { return m_DeviceRegistry != nullptr; }

    // Registry-based KWin refreshes outputs for an empty configuration, leaving
    // every setting owned by KWin. Legacy KWin needs an explicit change set;
    // re-send only the unchanged SDR reference after checking freshness.
    bool reapplyCurrentConfiguration(const std::vector<const OutputDevice*>& outputs, QString& failure)
    {
        struct Result {
            bool done = false;
            bool applied = false;
            QString reason;
        } result;
        static const kde_output_configuration_v2_listener configurationListener{
            [](void* data, kde_output_configuration_v2*) {
                auto* r = static_cast<Result*>(data);
                r->done = r->applied = true;
            },
            [](void* data, kde_output_configuration_v2*) {
                static_cast<Result*>(data)->done = true;
            },
            [](void* data, kde_output_configuration_v2*, const char* reason) {
                static_cast<Result*>(data)->reason = QString::fromUtf8(reason);
            },
        };
        auto* configuration = kde_output_management_v2_create_configuration(m_Manager);
        kde_output_configuration_v2_add_listener(configuration, &configurationListener, &result);
        std::vector<uint64_t> revisions;
        for (const OutputDevice* output : outputs) {
            revisions.push_back(output->revision);
        }
        // Establish the configuration on the server before checking freshness.
        // KWin rejects configurations when outputs are added/removed afterward.
        if (!roundtrip(m_Display, m_Deadline) || m_ManagerRemoved) {
            failure = QStringLiteral("output management became unavailable before apply");
            kde_output_configuration_v2_destroy(configuration);
            return false;
        }
        for (size_t i = 0; i < outputs.size(); i++) {
            const auto* output = outputs[i];
            if (output->removed || !output->complete || !output->enabled || !output->hdr ||
                    output->prefersAccuracy || output->revision != revisions[i]) {
                failure = QStringLiteral("output state changed before apply; left configuration untouched");
                kde_output_configuration_v2_destroy(configuration);
                return false;
            }
        }
        if (!usesRegistry()) {
            for (const OutputDevice* output : outputs) {
                kde_output_configuration_v2_set_sdr_brightness(configuration, output->proxy, output->sdrBrightness);
            }
        }
        kde_output_configuration_v2_apply(configuration);
        // KWin answers while handling the apply request.
        roundtrip(m_Display, m_Deadline);
        kde_output_configuration_v2_destroy(configuration);
        if (!result.applied) {
            failure = !result.reason.isEmpty() ? result.reason
                      : result.done ? QStringLiteral("KWin rejected the configuration")
                                    : QStringLiteral("no answer from KWin");
        }
        return result.applied;
    }

private:
    void addDevice(kde_output_device_v2* proxy, bool fromRegistry, uint32_t globalName)
    {
        auto device = std::make_unique<OutputDevice>();
        device->proxy = proxy;
        device->fromRegistry = fromRegistry;
        device->globalName = globalName;
        kde_output_device_v2_add_listener(device->proxy, outputDeviceListener(), device.get());
        m_Devices.push_back(std::move(device));
    }

    wl_display* m_Display = nullptr;
    wl_registry* m_Registry = nullptr;
    kde_output_device_registry_v2* m_DeviceRegistry = nullptr;
    kde_output_management_v2* m_Manager = nullptr;
    uint32_t m_ManagerGlobal = 0;
    bool m_ManagerRemoved = false;
    const std::chrono::steady_clock::time_point m_Deadline = std::chrono::steady_clock::now() + k_WaylandTimeout;
    std::vector<std::unique_ptr<OutputDevice>> m_Devices;
    QString m_Error;
};

bool drmCall(int fd, unsigned long request, void* argument)
{
    int ret;
    do {
        ret = ioctl(fd, request, argument);
    } while (ret < 0 && (errno == EINTR || errno == EAGAIN));
    return ret == 0;
}

// Property name -> value for one KMS object.
std::map<std::string, uint64_t> drmProperties(int fd, uint32_t objectId, uint32_t objectType)
{
    std::map<std::string, uint64_t> properties;
    drm_mode_obj_get_properties request = {};
    request.obj_id = objectId;
    request.obj_type = objectType;
    if (!drmCall(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &request)) {
        return properties;
    }
    std::vector<uint32_t> ids(request.count_props);
    std::vector<uint64_t> values(request.count_props);
    request.props_ptr = reinterpret_cast<uintptr_t>(ids.data());
    request.prop_values_ptr = reinterpret_cast<uintptr_t>(values.data());
    if (!drmCall(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &request)) {
        return properties;
    }
    const size_t count = std::min<size_t>(ids.size(), request.count_props);
    for (size_t i = 0; i < count; i++) {
        drm_mode_get_property property = {};
        property.prop_id = ids[i];
        if (drmCall(fd, DRM_IOCTL_MODE_GETPROPERTY, &property)) {
            properties[std::string(property.name, strnlen(property.name, DRM_PROP_NAME_LEN))] = values[i];
        }
    }
    return properties;
}

// The connector name KWin and the kernel use, e.g. "DP-1".
QString drmConnectorName(int fd, uint32_t connectorId)
{
    static const char* const typeNames[] = {
        "Unknown", "VGA", "DVI-I", "DVI-D", "DVI-A", "Composite", "SVIDEO", "LVDS", "Component",
        "DIN", "DP", "HDMI-A", "HDMI-B", "TV", "eDP", "Virtual", "DSI", "DPI", "Writeback", "SPI", "USB",
    };
    // Asking for one mode keeps the kernel from probing the connector.
    drm_mode_modeinfo mode = {};
    drm_mode_get_connector connector = {};
    connector.connector_id = connectorId;
    connector.count_modes = 1;
    connector.modes_ptr = reinterpret_cast<uintptr_t>(&mode);
    if (!drmCall(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector)) {
        return QStringLiteral("connector %1").arg(connectorId);
    }
    const char* type = connector.connector_type < std::size(typeNames) ? typeNames[connector.connector_type] : "Unknown";
    return QStringLiteral("%1-%2").arg(type).arg(connector.connector_type_id);
}

// Reads one card. Returns false when its KMS state cannot be read.
bool probeDrmCard(const char* path, QStringList& shadowOutputs, QStringList* activeHdrOutputs)
{
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    // Opening a primary node makes the opener DRM master when nobody holds it
    // (e.g. KWin's session is switched away). Closing at once gives it back.
    // Only master may authenticate, and magic 0 is never valid.
    drm_auth auth = {};
    if (ioctl(fd, DRM_IOCTL_AUTH_MAGIC, &auth) == 0 || errno != EACCES) {
        close(fd);
        return false;
    }
    // CRTC_ID and ACTIVE are atomic-only properties.
    drm_set_client_cap atomic = {DRM_CLIENT_CAP_ATOMIC, 1};
    drm_mode_card_res resources = {};
    if (!drmCall(fd, DRM_IOCTL_SET_CLIENT_CAP, &atomic) || !drmCall(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources)) {
        close(fd);
        return false;
    }
    std::vector<uint32_t> connectors(resources.count_connectors);
    drm_mode_card_res list = {};
    list.count_connectors = connectors.size();
    list.connector_id_ptr = reinterpret_cast<uintptr_t>(connectors.data());
    if (!drmCall(fd, DRM_IOCTL_MODE_GETRESOURCES, &list)) {
        close(fd);
        return false;
    }
    connectors.resize(std::min<size_t>(connectors.size(), list.count_connectors));
    for (uint32_t connectorId : connectors) {
        const auto connector = drmProperties(fd, connectorId, DRM_MODE_OBJECT_CONNECTOR);
        const auto crtcId = connector.find("CRTC_ID");
        const auto hdrMetadata = connector.find("HDR_OUTPUT_METADATA");
        // KWin sets HDR metadata only for PQ output; SDR leaves the blob empty.
        if (crtcId == connector.end() || !crtcId->second || hdrMetadata == connector.end() || !hdrMetadata->second) {
            continue;
        }
        const auto crtc = drmProperties(fd, uint32_t(crtcId->second), DRM_MODE_OBJECT_CRTC);
        const auto active = crtc.find("ACTIVE");
        const auto gammaLut = crtc.find("GAMMA_LUT");
        if (active != crtc.end() && active->second && gammaLut != crtc.end()) {
            const QString name = drmConnectorName(fd, connectorId);
            if (activeHdrOutputs) *activeHdrOutputs << name;
            if (!gammaLut->second) shadowOutputs << name;
        }
    }
    close(fd);
    return true;
}

#ifdef HAVE_QTDBUS
const QString k_KWinService = QStringLiteral("org.kde.KWin");
const QString k_EffectsPath = QStringLiteral("/Effects");
const QString k_EffectsInterface = QStringLiteral("org.kde.kwin.Effects");

QDBusMessage callKWinEffects(const QString& method, const QVariantList& arguments)
{
    QDBusMessage call = QDBusMessage::createMethodCall(k_KWinService, k_EffectsPath, k_EffectsInterface, method);
    call.setArguments(arguments);
    return QDBusConnection::sessionBus().call(call, QDBus::Block, 2000);
}

bool kwinActiveEffects(QStringList& effects, QString& error)
{
    QDBusMessage call = QDBusMessage::createMethodCall(k_KWinService, k_EffectsPath,
                                                       QStringLiteral("org.freedesktop.DBus.Properties"),
                                                       QStringLiteral("Get"));
    call.setArguments({k_EffectsInterface, QStringLiteral("activeEffects")});
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 2000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        error = reply.errorMessage().isEmpty() || reply.errorMessage() == reply.errorName()
                    ? reply.errorName() : reply.errorName() + QStringLiteral(": ") + reply.errorMessage();
        return false;
    }
    effects = qvariant_cast<QDBusVariant>(reply.arguments().constFirst()).variant().toStringList();
    return true;
}
#endif

}

bool KWinScanoutGuard::StuckEffects::isTransientAnimation(const QString& effect)
{
    // Effects that animate a window or popup, or a desktop switch, for well
    // under a second. Effects that stay active by design (zoom, dim inactive,
    // blur, ...) or that follow DPMS (kscreen) are deliberately absent.
    static const QStringList transient = {
        QStringLiteral("blendchanges"), QStringLiteral("fade"), QStringLiteral("fadedesktop"),
        QStringLiteral("fadingpopups"), QStringLiteral("glide"), QStringLiteral("magiclamp"),
        QStringLiteral("maximize"), QStringLiteral("morphingpopups"), QStringLiteral("scale"),
        QStringLiteral("slide"), QStringLiteral("slidingpopups"), QStringLiteral("squash"),
        QStringLiteral("windowaperture"),
    };
    return transient.contains(effect);
}

QStringList KWinScanoutGuard::StuckEffects::update(Clock::time_point now, const QStringList& activeEffects)
{
    QStringList stuck;
    for (auto it = m_FirstSeen.begin(); it != m_FirstSeen.end();) {
        it = activeEffects.contains(it->first) ? std::next(it) : m_FirstSeen.erase(it);
    }
    for (const QString& effect : activeEffects) {
        if (!isTransientAnimation(effect) || m_Reloads[effect] >= k_MaxReloadsPerEffect) {
            continue;
        }
        auto& firstSeen = m_FirstSeen.emplace(effect, now).first->second;
        if (now - firstSeen >= k_StuckAfter) {
            stuck << effect;
            m_Reloads[effect]++;
            firstSeen = now;
        }
    }
    return stuck;
}

KWinScanoutGuard::HdrOffload KWinScanoutGuard::probeHdrOffload(QStringList* shadowOutputs)
{
    return probeHdrOffload(shadowOutputs, nullptr);
}

KWinScanoutGuard::HdrOffload KWinScanoutGuard::probeHdrOffload(QStringList* shadowOutputs, QStringList* activeHdrOutputs)
{
    QStringList outputs;
    if (activeHdrOutputs) activeHdrOutputs->clear();
    bool readable = false;
    if (DIR* dir = opendir("/dev/dri")) {
        while (const dirent* entry = readdir(dir)) {
            if (strncmp(entry->d_name, "card", 4) == 0) {
                const std::string path = std::string("/dev/dri/") + entry->d_name;
                readable |= probeDrmCard(path.c_str(), outputs, activeHdrOutputs);
            }
        }
        closedir(dir);
    }
    if (shadowOutputs) {
        *shadowOutputs = outputs;
    }
    if (!outputs.isEmpty()) {
        return HdrOffload::ShadowBuffer;
    }
    return readable ? HdrOffload::Ok : HdrOffload::Unknown;
}

QStringList KWinScanoutGuard::HdrRecovery::update(Clock::time_point now, HdrOffload state,
                                                QStringList outputs, bool fullscreen)
{
    if (m_Attempted) return {};
    outputs.removeDuplicates();
    outputs.sort();
    if (!fullscreen || state != HdrOffload::ShadowBuffer || outputs.isEmpty()) {
        m_Pending.clear();
        return {};
    }
    if (outputs != m_Pending) {
        m_Pending = outputs;
        m_FirstSeen = now;
        return {};
    }
    if (now - m_FirstSeen < k_StableFor) return {};
    m_Attempted = true;
    return outputs;
}

KWinScanoutGuard::HdrRetryResult KWinScanoutGuard::retryHdrColorOffload(const QStringList& outputs, int connectedFd)
{
    HdrRetryResult result;
    OutputManagement management(connectedFd);
    if (!management.ok()) {
        result.failure = management.error();
        return result;
    }
    result.available = true;
    if (management.devices().empty()) {
        result.failure = QStringLiteral("KWin announced no output devices");
        return result;
    }
    std::vector<const OutputDevice*> targets;
    QStringList targetDescriptions;
    for (const auto& device : management.devices()) {
        if (management.usesRegistry() && !device->fromRegistry) continue;
        if (device->removed || !device->complete || device->name.isEmpty()) continue;
        if (!device->enabled || (!outputs.isEmpty() && !outputs.contains(device->name))) {
            continue;
        }
        if (!device->hdr) {
            result.skipped << device->name + QStringLiteral(" (SDR)");
        }
        else if (device->prefersAccuracy) {
            // KWin then uses the shadow buffer on purpose.
            result.skipped << device->name + QStringLiteral(" (prefers color accuracy)");
        }
        else if (!device->sdrBrightness) {
            result.skipped << device->name + QStringLiteral(" (no SDR brightness)");
        }
        else {
            const auto duplicates = std::count_if(management.devices().begin(), management.devices().end(), [&](const auto& other) {
                return !other->removed && other->complete && other->enabled && other->name == device->name &&
                       (!management.usesRegistry() || other->fromRegistry);
            });
            if (duplicates != 1) {
                result.skipped << device->name + QStringLiteral(" (ambiguous output name)");
                continue;
            }
            targets.push_back(device.get());
            targetDescriptions << QStringLiteral("%1 at %2 nits").arg(device->name).arg(device->sdrBrightness);
        }
    }
    if (!targets.empty()) {
        result.applied = management.reapplyCurrentConfiguration(targets, result.failure);
        if (result.applied) result.retried = targetDescriptions;
    }
    else if (result.skipped.isEmpty() && !outputs.isEmpty()) {
        result.failure = QStringLiteral("no enabled KWin output matched KMS connector(s): %1")
                             .arg(outputs.join(QStringLiteral(", ")));
    }
    return result;
}

std::unique_ptr<KWinScanoutGuard> KWinScanoutGuard::start(bool fullscreen)
{
    if (qEnvironmentVariableIsSet("MOONLIGHT_KWIN_SCANOUT_GUARD") &&
            qEnvironmentVariableIntValue("MOONLIGHT_KWIN_SCANOUT_GUARD") == 0) {
        qInfo("KWin scanout guard: disabled by MOONLIGHT_KWIN_SCANOUT_GUARD=0");
        return nullptr;
    }
    if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        return nullptr;
    }
    return std::unique_ptr<KWinScanoutGuard>(new KWinScanoutGuard(fullscreen));
}

KWinScanoutGuard::KWinScanoutGuard(bool fullscreen)
    : m_Fullscreen(fullscreen)
{
    m_Thread = std::thread(&KWinScanoutGuard::run, this);
}

KWinScanoutGuard::~KWinScanoutGuard()
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Stop = true;
    }
    m_Wake.notify_all();
    if (m_Thread.joinable()) m_Thread.join();
}

bool KWinScanoutGuard::waitFor(std::chrono::steady_clock::duration duration)
{
    std::unique_lock<std::mutex> lock(m_Mutex);
    return m_Wake.wait_for(lock, duration, [this] { return m_Stop; });
}

void KWinScanoutGuard::run()
{
    using Clock = std::chrono::steady_clock;

    // Only KWin offers kde_output_management_v2, so this also tells KWin apart
    // from other Wayland compositors (including Gamescope).
    {
        OutputManagement management(-1);
        if (!management.ok()) {
            qInfo() << "KWin scanout guard: inactive," << management.error();
            return;
        }
    }

    bool watchEffects = false;
    QString effectsStatus = QStringLiteral("unavailable (built without Qt D-Bus)");
#ifdef HAVE_QTDBUS
    {
        QStringList active;
        QString error;
        watchEffects = kwinActiveEffects(active, error);
        effectsStatus = watchEffects ? QStringLiteral("active now: [%1]").arg(active.join(QStringLiteral(", ")))
                                     : QStringLiteral("unavailable (%1; a Flatpak needs --talk-name=org.kde.KWin)").arg(error);
    }
#endif

    QStringList shadowOutputs;
    const HdrOffload initialHdr = probeHdrOffload(&shadowOutputs);
    // Keep observing if KMS is temporarily unreadable during output hotplug.
    bool checkHdr = true;
    qInfo().noquote() << "KWin scanout guard: effects" << effectsStatus << "| HDR color offload:"
                      << (initialHdr == HdrOffload::Ok ? QStringLiteral("ok")
                          : initialHdr == HdrOffload::ShadowBuffer ? QStringLiteral("shadow buffer on ") + shadowOutputs.join(QStringLiteral(", "))
                                                                   : QStringLiteral("unknown (KMS state unreadable)"));

#ifdef HAVE_QTDBUS
    StuckEffects stuckEffects;
    int effectQueryFailures = 0;
#endif
    HdrRecovery hdrRecovery;
    auto nextHdrCheck = Clock::now();
    do {
        const auto now = Clock::now();
        if (checkHdr && now >= nextHdrCheck) {
            const HdrOffload state = probeHdrOffload(&shadowOutputs);
            const QStringList targets = hdrRecovery.update(now, state, shadowOutputs, m_Fullscreen.load());
            nextHdrCheck = now + (state == HdrOffload::ShadowBuffer ? k_HdrSettleTime : k_HdrCheckInterval);
            if (!targets.isEmpty()) {
                // Consume the stream budget before making any request, including
                // on rejection, timeout, output removal or an uncertain reply.
                checkHdr = false;
                const HdrRetryResult retry = retryHdrColorOffload(targets);
                QString message = QStringLiteral("KWin scanout guard: stable fullscreen HDR fallback on %1. ")
                                      .arg(targets.join(QStringLiteral(", ")));
                if (!retry.retried.isEmpty()) {
                    message += QStringLiteral("KWin accepted an unchanged output configuration for %1")
                                   .arg(retry.retried.join(QStringLiteral(", ")));
                }
                else {
                    message += QStringLiteral("No output configuration was confirmed applied");
                }
                if (!retry.skipped.isEmpty()) {
                    message += QStringLiteral("; left alone: ") + retry.skipped.join(QStringLiteral(", "));
                }
                if (!retry.failure.isEmpty()) {
                    message += QStringLiteral("; failed: ") + retry.failure;
                }
                message += QStringLiteral("; no further automatic HDR applies this stream");
                qWarning().noquote() << message;
                if (retry.applied) {
                    if (waitFor(k_HdrSettleTime)) break;
                    QStringList stillShadowed;
                    QStringList activeHdrOutputs;
                    const HdrOffload after = probeHdrOffload(&stillShadowed, &activeHdrOutputs);
                    QStringList stillAffected;
                    QStringList unavailable;
                    for (const auto& target : targets) {
                        if (stillShadowed.contains(target)) stillAffected << target;
                        if (!activeHdrOutputs.contains(target)) unavailable << target;
                    }
                    qInfo().noquote() << "KWin scanout guard: HDR color offload after retry:"
                                      << (after == HdrOffload::Unknown ? QStringLiteral("unknown (KMS state unreadable)")
                                          : !unavailable.isEmpty() ? QStringLiteral("output no longer active HDR: ") + unavailable.join(QStringLiteral(", "))
                                          : stillAffected.isEmpty() ? QStringLiteral("ok on requested outputs")
                                          : QStringLiteral("still shadow buffer on ") + stillAffected.join(QStringLiteral(", ")));
                }
            }
        }

#ifdef HAVE_QTDBUS
        if (watchEffects && !m_Fullscreen) {
            stuckEffects.reset();
        }
        else if (watchEffects) {
            QStringList active;
            QString error;
            if (!kwinActiveEffects(active, error)) {
                if (++effectQueryFailures >= k_MaxEffectQueryFailures) {
                    qWarning().noquote() << "KWin scanout guard: stopped watching effects:" << error;
                    watchEffects = false;
                }
            }
            else {
                effectQueryFailures = 0;
                for (const QString& effect : stuckEffects.update(Clock::now(), active)) {
                    const QString state = callKWinEffects(QStringLiteral("debug"), {effect, QString()})
                                              .arguments().value(0).toString().simplified();
                    qWarning().noquote() << QStringLiteral("KWin scanout guard: effect %1 has been active for at least %2 s, "
                                                           "which blocks direct scanout (%3); reloading it")
                                                .arg(effect).arg(StuckEffects::k_StuckAfter.count()).arg(state);
                    callKWinEffects(QStringLiteral("unloadEffect"), {effect});
                    callKWinEffects(QStringLiteral("loadEffect"), {effect});
                }
            }
        }
#endif
    } while ((checkHdr || watchEffects) && !waitFor(k_PollInterval));
}
