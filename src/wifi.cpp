#include "wifi.hpp"

#include <NetworkManager.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace cyberdeck {
namespace {
void configure_pipe(int fd) {
  if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0 ||
      fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC) < 0)
    throw std::runtime_error("cannot configure WiFi notification pipe");
}

std::string ssid_text(GBytes *bytes) {
  if (!bytes) return {};
  gsize length = 0;
  const auto *data = static_cast<const guint8 *>(g_bytes_get_data(bytes, &length));
  if (!data || !length) return {};
  char *converted = nm_utils_ssid_to_utf8(data, length);
  std::string result = converted ? converted : "";
  g_free(converted);
  for (char &ch : result)
    if (static_cast<unsigned char>(ch) < 32 || ch == 127) ch = '?';
  return result;
}

WifiSecurity security_of(NMAccessPoint *ap) {
  auto keys = static_cast<unsigned>(nm_access_point_get_wpa_flags(ap)) |
              static_cast<unsigned>(nm_access_point_get_rsn_flags(ap));
  if (keys & (NM_802_11_AP_SEC_KEY_MGMT_PSK | NM_802_11_AP_SEC_KEY_MGMT_SAE))
    return WifiSecurity::Personal;
  if (keys & NM_802_11_AP_SEC_KEY_MGMT_802_1X) return WifiSecurity::Enterprise;
  auto flags = nm_access_point_get_flags(ap);
  if (!(flags & NM_802_11_AP_FLAGS_PRIVACY)) return WifiSecurity::Open;
  return WifiSecurity::Unsupported;
}

bool ap_uses_sae(NMAccessPoint *ap) {
  const auto keys = static_cast<unsigned>(nm_access_point_get_wpa_flags(ap)) |
                    static_cast<unsigned>(nm_access_point_get_rsn_flags(ap));
  return !(keys & NM_802_11_AP_SEC_KEY_MGMT_PSK) &&
         (keys & NM_802_11_AP_SEC_KEY_MGMT_SAE);
}

bool profile_uses_sae(NMConnection *profile) {
  auto *security = nm_connection_get_setting_wireless_security(profile);
  const char *method = security ? nm_setting_wireless_security_get_key_mgmt(security) : nullptr;
  return method && std::strcmp(method, "sae") == 0;
}

std::string signature(const WifiSnapshot &value) {
  std::string result = value.status + value.device + value.connected_ssid +
      std::to_string(value.available) + std::to_string(value.enabled) +
      std::to_string(value.hardware_enabled) + std::to_string(value.scanning) +
      std::to_string(value.connecting) + std::to_string(value.forgetting) +
      std::to_string(value.forget_epoch);
  for (const auto &network : value.networks)
    result += network.path + network.ssid + network.saved_uuid +
              std::to_string(network.strength) + std::to_string(network.connected) +
              std::to_string(network.visible) +
              std::to_string(static_cast<int>(network.security)) +
              std::to_string(network.uses_sae);
  return result;
}
}  // namespace

struct WifiBackend::Impl {
  enum class Action { Scan, Connect, Disconnect, Enable, Forget };
  struct Command {
    Action action;
    WifiNetwork network;
    std::string password;
    bool enabled = false;
  };

  int pipe_fd[2] = {-1, -1};
  std::thread worker;
  std::mutex mutex;
  std::deque<Command> commands;
  WifiSnapshot view;
  bool stop = false;
  GMainContext *context = nullptr;  // Accessed under mutex when posted from the UI.
  NMClient *client = nullptr;       // Worker thread only.
  NMActiveConnection *pending_active = nullptr;
  bool scanning = false;
  bool connecting = false;
  bool forgetting = false;
  uint64_t forget_epoch = 0;
  unsigned forget_pending = 0;
  unsigned forget_count = 0;
  bool forget_disconnect_needed = false;
  bool forget_disconnect_pending = false;
  GBytes *forget_ssid = nullptr;
  std::string forget_uuid, forget_name, forget_error;
  std::chrono::steady_clock::time_point forget_cache_deadline{};
  std::string message;
  std::string target_ssid;
  std::string target_ap_path;
  gint64 scan_before = -1;
  std::chrono::steady_clock::time_point scan_deadline{};
  std::chrono::steady_clock::time_point connect_deadline{};

  Impl() {
    if (::pipe(pipe_fd) < 0) throw std::runtime_error("cannot create WiFi notification pipe");
    try {
      configure_pipe(pipe_fd[0]);
      configure_pipe(pipe_fd[1]);
      worker = std::thread([this] { run(); });
    } catch (...) {
      ::close(pipe_fd[0]);
      ::close(pipe_fd[1]);
      throw;
    }
  }

  ~Impl() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stop = true;
      if (context) g_main_context_wakeup(context);
    }
    if (worker.joinable()) worker.join();
    ::close(pipe_fd[0]);
    ::close(pipe_fd[1]);
  }

  void post(Command command) {
    std::lock_guard<std::mutex> lock(mutex);
    if (commands.size() >= 16) {
      if (command.action != Action::Forget) return;
      std::fill(commands.front().password.begin(), commands.front().password.end(), '\0');
      commands.pop_front();
    }
    commands.push_back(std::move(command));
    if (context) g_main_context_wakeup(context);
  }

  void publish(WifiSnapshot fresh) {
    std::lock_guard<std::mutex> lock(mutex);
    if (signature(view) == signature(fresh)) return;
    view = std::move(fresh);
    const char byte = 1;
    (void)::write(pipe_fd[1], &byte, 1);
  }

  NMDeviceWifi *device() const {
    if (!client) return nullptr;
    const GPtrArray *devices = nm_client_get_devices(client);
    if (!devices) return nullptr;
    for (guint i = 0; i < devices->len; ++i) {
      auto *candidate = static_cast<NMDevice *>(g_ptr_array_index(devices, i));
      if (NM_IS_DEVICE_WIFI(candidate)) return NM_DEVICE_WIFI(candidate);
    }
    return nullptr;
  }

  NMAccessPoint *access_point(NMDeviceWifi *wifi, const std::string &path) const {
    if (!wifi || path.empty()) return nullptr;
    return nm_device_wifi_get_access_point_by_path(wifi, path.c_str());
  }

  bool matches_forget(NMRemoteConnection *profile) const {
    auto *wireless = nm_connection_get_setting_wireless(NM_CONNECTION(profile));
    if (!wireless) return false;
    if (forget_ssid) {
      GBytes *ssid = nm_setting_wireless_get_ssid(wireless);
      return ssid && g_bytes_equal(ssid, forget_ssid);
    }
    const char *uuid = nm_connection_get_uuid(NM_CONNECTION(profile));
    return uuid && forget_uuid == uuid;
  }

  unsigned remaining_forget_profiles() const {
    if (!client) return 0;
    const GPtrArray *connections = nm_client_get_connections(client);
    if (!connections) return 0;
    unsigned count = 0;
    for (guint i = 0; i < connections->len; ++i)
      if (matches_forget(static_cast<NMRemoteConnection *>(g_ptr_array_index(connections, i)))) ++count;
    return count;
  }

  bool active_matches_forget(NMDeviceWifi *wifi) const {
    if (!wifi) return false;
    NMAccessPoint *active = nm_device_wifi_get_active_access_point(wifi);
    if (!active || nm_device_get_state(NM_DEVICE(wifi)) != NM_DEVICE_STATE_ACTIVATED) return false;
    if (forget_ssid) {
      GBytes *ssid = nm_access_point_get_ssid(active);
      return ssid && g_bytes_equal(ssid, forget_ssid);
    }
    NMActiveConnection *connection = nm_device_get_active_connection(NM_DEVICE(wifi));
    const char *uuid = connection ? nm_active_connection_get_uuid(connection) : nullptr;
    return uuid && forget_uuid == uuid;
  }

  static void forget_disconnect_finished(GObject *source, GAsyncResult *result, gpointer data) {
    auto *self = static_cast<Impl *>(data);
    GError *error = nullptr;
    if (!nm_device_disconnect_finish(NM_DEVICE(source), result, &error))
      self->forget_error = error ? error->message : "Could not disconnect";
    g_clear_error(&error);
    self->forget_disconnect_pending = false;
    self->maybe_finish_forget();
    self->update();
  }

  void maybe_finish_forget() {
    if (!forgetting || forget_pending || forget_disconnect_pending) return;
    if (forget_error.empty() && forget_disconnect_needed) {
      forget_disconnect_needed = false;
      NMDeviceWifi *wifi = device();
      if (active_matches_forget(wifi)) {
        forget_disconnect_pending = true;
        nm_device_disconnect_async(NM_DEVICE(wifi), nullptr, forget_disconnect_finished, this);
        return;
      }
    }
    const unsigned remaining = forget_error.empty() ? remaining_forget_profiles() : 0;
    if (remaining && std::chrono::steady_clock::now() < forget_cache_deadline) return;
    if (remaining) forget_error = "Saved profiles still present; try Forget again";
    message = forget_error.empty() ? "Forgot " + forget_name + " (" +
              std::to_string(forget_count) + " profiles)" : "Forget failed: " + forget_error;
    forgetting = false;
    ++forget_epoch;
    g_clear_pointer(&forget_ssid, g_bytes_unref);
    forget_uuid.clear();
    forget_name.clear();
    forget_error.clear();
    forget_count = 0;
  }

  std::string matching_profile(NMAccessPoint *ap, WifiSecurity security) const {
    if (!client || !ap) return {};
    const GPtrArray *connections = nm_client_get_connections(client);
    if (!connections) return {};
    std::string best_uuid;
    guint64 best_timestamp = 0;
    for (guint i = 0; i < connections->len; ++i) {
      auto *profile = static_cast<NMRemoteConnection *>(g_ptr_array_index(connections, i));
      auto *wireless = nm_connection_get_setting_wireless(NM_CONNECTION(profile));
      if (!wireless || !nm_setting_wireless_get_ssid(wireless) ||
          !nm_access_point_get_ssid(ap) ||
          !g_bytes_equal(nm_setting_wireless_get_ssid(wireless), nm_access_point_get_ssid(ap)))
        continue;
      bool protected_profile = nm_connection_get_setting_wireless_security(NM_CONNECTION(profile));
      if ((security == WifiSecurity::Open) == protected_profile) continue;
      if (security == WifiSecurity::Personal) {
        auto *wireless_security = nm_connection_get_setting_wireless_security(NM_CONNECTION(profile));
        const char *method = wireless_security ?
            nm_setting_wireless_security_get_key_mgmt(wireless_security) : nullptr;
        if (!method || (std::strcmp(method, "wpa-psk") != 0 && std::strcmp(method, "sae") != 0))
          continue;
      }
      const char *uuid = nm_connection_get_uuid(NM_CONNECTION(profile));
      auto *settings = nm_connection_get_setting_connection(NM_CONNECTION(profile));
      guint64 timestamp = settings ? nm_setting_connection_get_timestamp(settings) : 0;
      if (uuid && (best_uuid.empty() || timestamp > best_timestamp)) {
        best_uuid = uuid;
        best_timestamp = timestamp;
      }
    }
    return best_uuid;
  }

  void update() {
    maybe_finish_forget();
    WifiSnapshot fresh;
    fresh.forgetting = forgetting;
    fresh.forget_epoch = forget_epoch;
    if (!client || !nm_client_get_nm_running(client)) {
      fresh.status = "NetworkManager is not running";
      publish(std::move(fresh));
      return;
    }
    fresh.available = true;
    fresh.enabled = nm_client_wireless_get_enabled(client);
    fresh.hardware_enabled = nm_client_wireless_hardware_get_enabled(client);
    fresh.scanning = scanning;
    fresh.connecting = connecting;
    if (connecting && pending_active &&
        nm_active_connection_get_state(pending_active) == NM_ACTIVE_CONNECTION_STATE_DEACTIVATED) {
      connecting = false;
      fresh.connecting = false;
      message = "Connection failed; check password or network";
      g_clear_object(&pending_active);
    }
    NMDeviceWifi *wifi = device();
    if (!wifi) {
      fresh.status = "No WiFi adapter found";
      publish(std::move(fresh));
      return;
    }
    const char *iface = nm_device_get_iface(NM_DEVICE(wifi));
    fresh.device = iface ? iface : "";
    if (scanning && (nm_device_wifi_get_last_scan(wifi) > scan_before ||
                     std::chrono::steady_clock::now() > scan_deadline)) scanning = false;
    fresh.scanning = scanning;
    if (!fresh.hardware_enabled) fresh.status = "WiFi blocked by hardware switch";
    else if (!fresh.enabled) fresh.status = "WiFi is off";
    else if (!nm_device_get_managed(NM_DEVICE(wifi))) fresh.status = "Adapter is unmanaged";
    else if (connecting && std::chrono::steady_clock::now() > connect_deadline) {
      connecting = false;
      fresh.connecting = false;
      message = "Connection timed out";
      fresh.status = message;
      g_clear_object(&pending_active);
    } else if (connecting) fresh.status = "Connecting...";
    else if (forgetting) fresh.status = "Forgetting saved profiles...";
    else if (!message.empty()) fresh.status = message;
    else fresh.status = "Disconnected";

    NMAccessPoint *active = nm_device_wifi_get_active_access_point(wifi);
    if (active && nm_device_get_state(NM_DEVICE(wifi)) == NM_DEVICE_STATE_ACTIVATED) {
      const std::string active_ssid = ssid_text(nm_access_point_get_ssid(active));
      fresh.connected_ssid = active_ssid;
      if (connecting && pending_active &&
          nm_active_connection_get_state(pending_active) == NM_ACTIVE_CONNECTION_STATE_ACTIVATED &&
          active_ssid == target_ssid) {
        connecting = false;
        fresh.connecting = false;
        message.clear();
        g_clear_object(&pending_active);
      }
      if (!connecting && !forgetting && message.empty()) fresh.status = "Connected";
    }
    const GPtrArray *points = nm_device_wifi_get_access_points(wifi);
    if (points && fresh.enabled) {
      for (guint i = 0; i < points->len; ++i) {
        auto *ap = static_cast<NMAccessPoint *>(g_ptr_array_index(points, i));
        WifiNetwork network;
        network.ssid = ssid_text(nm_access_point_get_ssid(ap));
        if (network.ssid.empty()) continue;  // Hidden networks need a separate flow.
        const char *path = nm_object_get_path(NM_OBJECT(ap));
        if (!path) continue;
        network.path = path;
        network.strength = nm_access_point_get_strength(ap);
        network.security = security_of(ap);
        network.uses_sae = network.security == WifiSecurity::Personal && ap_uses_sae(ap);
        network.saved_uuid = matching_profile(ap, network.security);
        network.connected = active == ap && nm_device_get_state(NM_DEVICE(wifi)) == NM_DEVICE_STATE_ACTIVATED;
        if (network.connected) {
          NMActiveConnection *current = nm_device_get_active_connection(NM_DEVICE(wifi));
          const char *uuid = current ? nm_active_connection_get_uuid(current) : nullptr;
          network.saved_uuid = uuid && nm_client_get_connection_by_uuid(client, uuid) ? uuid : "";
        }
        if (!network.saved_uuid.empty()) {
          NMRemoteConnection *profile = nm_client_get_connection_by_uuid(client, network.saved_uuid.c_str());
          if (profile) network.uses_sae = profile_uses_sae(NM_CONNECTION(profile));
        }
        auto same = std::find_if(fresh.networks.begin(), fresh.networks.end(), [&](const WifiNetwork &old) {
          return old.ssid == network.ssid && old.security == network.security;
        });
        if (same == fresh.networks.end()) fresh.networks.push_back(std::move(network));
        else if (network.connected || (!same->connected && network.strength > same->strength))
          *same = std::move(network);
      }
    }
    const GPtrArray *profiles = nm_client_get_connections(client);
    if (profiles) {
      for (guint i = 0; i < profiles->len; ++i) {
        auto *profile = static_cast<NMRemoteConnection *>(g_ptr_array_index(profiles, i));
        auto *wireless = nm_connection_get_setting_wireless(NM_CONNECTION(profile));
        const char *uuid = nm_connection_get_uuid(NM_CONNECTION(profile));
        if (!wireless || !uuid) continue;
        const bool already_shown = std::any_of(fresh.networks.begin(), fresh.networks.end(),
            [uuid](const WifiNetwork &network) { return network.saved_uuid == uuid; });
        if (already_shown) continue;
        WifiNetwork saved;
        saved.ssid = ssid_text(nm_setting_wireless_get_ssid(wireless));
        if (saved.ssid.empty()) {
          const char *id = nm_connection_get_id(NM_CONNECTION(profile));
          saved.ssid = id ? id : "Hidden network";
        }
        saved.saved_uuid = uuid;
        saved.visible = false;
        saved.security = nm_connection_get_setting_wireless_security(NM_CONNECTION(profile)) ?
            WifiSecurity::Personal : WifiSecurity::Open;
        saved.uses_sae = profile_uses_sae(NM_CONNECTION(profile));
        fresh.networks.push_back(std::move(saved));
      }
    }
    std::sort(fresh.networks.begin(), fresh.networks.end(), [](const WifiNetwork &a, const WifiNetwork &b) {
      if (a.connected != b.connected) return a.connected;
      if (a.visible != b.visible) return a.visible;
      if (a.strength != b.strength) return a.strength > b.strength;
      if (a.ssid != b.ssid) return a.ssid < b.ssid;
      return a.saved_uuid < b.saved_uuid;
    });
    publish(std::move(fresh));
  }

  static void scan_finished(GObject *source, GAsyncResult *result, gpointer data) {
    auto *self = static_cast<Impl *>(data);
    GError *error = nullptr;
    const bool accepted = nm_device_wifi_request_scan_finish(NM_DEVICE_WIFI(source), result, &error);
    if (!accepted) {
      self->message = error ? error->message : "Scan failed";
      g_clear_error(&error);
    } else self->message.clear();
    if (!accepted) self->scanning = false;
    self->update();
  }

  static void enabled_finished(GObject *source, GAsyncResult *result, gpointer data) {
    auto *self = static_cast<Impl *>(data);
    GError *error = nullptr;
    if (!nm_client_dbus_set_property_finish(NM_CLIENT(source), result, &error))
      self->message = error ? error->message : "Could not change WiFi state";
    g_clear_error(&error);
    self->update();
  }

  static void activation_finished(GObject *source, GAsyncResult *result, gpointer data) {
    auto *self = static_cast<Impl *>(data);
    GError *error = nullptr;
    NMActiveConnection *active = nm_client_activate_connection_finish(NM_CLIENT(source), result, &error);
    if (!active) {
      self->connecting = false;
      self->message = error ? error->message : "Connection failed";
    }
    if (active) {
      g_clear_object(&self->pending_active);
      self->pending_active = active;
    }
    g_clear_error(&error);
    self->update();
  }

  static void new_activation_finished(GObject *source, GAsyncResult *result, gpointer data) {
    auto *self = static_cast<Impl *>(data);
    GError *error = nullptr;
    NMActiveConnection *active = nm_client_add_and_activate_connection_finish(NM_CLIENT(source), result, &error);
    if (!active) {
      self->connecting = false;
      self->message = error ? error->message : "Connection failed";
    }
    if (active) {
      g_clear_object(&self->pending_active);
      self->pending_active = active;
    }
    g_clear_error(&error);
    self->update();
  }

  static void password_updated(GObject *source, GAsyncResult *result, gpointer data) {
    auto *self = static_cast<Impl *>(data);
    GError *error = nullptr;
    GVariant *response = nm_remote_connection_update2_finish(NM_REMOTE_CONNECTION(source), result, &error);
    if (!response) {
      self->connecting = false;
      self->message = error ? error->message : "Could not update saved password";
    } else if (self->forgetting) {
      self->connecting = false;
    } else {
      NMDeviceWifi *wifi = self->device();
      NMAccessPoint *ap = self->access_point(wifi, self->target_ap_path);
      if (!ap) {
        self->connecting = false;
        self->message = "Password saved; network disappeared, rescan";
      } else {
        const char *path = nm_object_get_path(NM_OBJECT(ap));
        nm_client_activate_connection_async(self->client, NM_CONNECTION(source), NM_DEVICE(wifi),
                                             path, nullptr, activation_finished, self);
      }
    }
    if (response) g_variant_unref(response);
    g_clear_error(&error);
    self->update();
  }

  static void disconnect_finished(GObject *source, GAsyncResult *result, gpointer data) {
    auto *self = static_cast<Impl *>(data);
    GError *error = nullptr;
    if (!nm_device_disconnect_finish(NM_DEVICE(source), result, &error))
      self->message = error ? error->message : "Disconnect failed";
    g_clear_error(&error);
    self->update();
  }

  static void forget_finished(GObject *source, GAsyncResult *result, gpointer data) {
    auto *self = static_cast<Impl *>(data);
    GError *error = nullptr;
    if (!nm_remote_connection_delete_finish(NM_REMOTE_CONNECTION(source), result, &error) &&
        self->forget_error.empty())
      self->forget_error = error ? error->message : "Could not forget network";
    g_clear_error(&error);
    if (self->forget_pending) --self->forget_pending;
    if (!self->forget_pending)
      self->forget_cache_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    self->maybe_finish_forget();
    self->update();
  }

  void handle(Command &command) {
    if (!client || !nm_client_get_nm_running(client)) {
      message = "NetworkManager unavailable";
      if (command.action == Action::Forget) ++forget_epoch;
      update();
      return;
    }
    if (command.action == Action::Forget) {
      if (forgetting) return;
      NMRemoteConnection *profile = nm_client_get_connection_by_uuid(client, command.network.saved_uuid.c_str());
      if (profile) {
        auto *wireless = nm_connection_get_setting_wireless(NM_CONNECTION(profile));
        GBytes *ssid = wireless ? nm_setting_wireless_get_ssid(wireless) : nullptr;
        forget_ssid = ssid ? g_bytes_ref(ssid) : nullptr;
        forget_uuid = command.network.saved_uuid;
        forget_name = command.network.ssid;
        forget_error.clear();
        forget_disconnect_needed = active_matches_forget(device());
        forget_disconnect_pending = false;
        std::vector<NMRemoteConnection *> profiles;
        const GPtrArray *all = nm_client_get_connections(client);
        if (all) for (guint i = 0; i < all->len; ++i) {
          auto *candidate = static_cast<NMRemoteConnection *>(g_ptr_array_index(all, i));
          if (matches_forget(candidate))
            profiles.push_back(NM_REMOTE_CONNECTION(g_object_ref(candidate)));
        }
        forgetting = true;
        forget_pending = static_cast<unsigned>(profiles.size());
        forget_count = forget_pending;
        message.clear();
        update();
        for (auto *candidate : profiles) {
          nm_remote_connection_delete_async(candidate, nullptr, forget_finished, this);
          g_object_unref(candidate);
        }
      } else {
        message = "Saved profile unavailable";
        ++forget_epoch;
      }
      update();
      return;
    }
    if (forgetting && command.action == Action::Connect) {
      message = "Wait for Forget to finish";
      update();
      return;
    }
    NMDeviceWifi *wifi = device();
    if (!wifi) {
      message = "WiFi adapter unavailable";
      update();
      return;
    }
    if (command.action == Action::Enable) {
      message.clear();
      nm_client_dbus_set_property(client, NM_DBUS_PATH, NM_DBUS_INTERFACE,
                                  "WirelessEnabled", g_variant_new("(b)", command.enabled),
                                  -1, nullptr, enabled_finished, this);
    } else if (command.action == Action::Scan) {
      if (!scanning && nm_client_wireless_get_enabled(client)) {
        scanning = true;
        scan_before = nm_device_wifi_get_last_scan(wifi);
        scan_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        message.clear();
        nm_device_wifi_request_scan_async(wifi, nullptr, scan_finished, this);
      }
    } else if (command.action == Action::Disconnect) {
      connecting = false;
      g_clear_object(&pending_active);
      message.clear();
      nm_device_disconnect_async(NM_DEVICE(wifi), nullptr, disconnect_finished, this);
    } else if (command.action == Action::Connect && !connecting) {
      NMAccessPoint *ap = access_point(wifi, command.network.path);
      if (!ap) { message = "Network disappeared; rescan"; update(); return; }
      connecting = true;
      g_clear_object(&pending_active);
      target_ssid = command.network.ssid;
      target_ap_path = command.network.path;
      connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
      message.clear();
      const char *path = nm_object_get_path(NM_OBJECT(ap));
      if (!command.network.saved_uuid.empty()) {
        NMRemoteConnection *profile = nm_client_get_connection_by_uuid(client, command.network.saved_uuid.c_str());
        if (!profile) { connecting = false; message = "Saved profile unavailable"; update(); return; }
        if (command.password.empty()) {
          nm_client_activate_connection_async(client, NM_CONNECTION(profile), NM_DEVICE(wifi), path,
                                               nullptr, activation_finished, this);
        } else {
          NMConnection *copy = nm_simple_connection_new_clone(NM_CONNECTION(profile));
          NMSettingWirelessSecurity *security = nm_connection_get_setting_wireless_security(copy);
          if (!security) {
            connecting = false;
            message = "Saved profile has no password setting";
          } else {
            g_object_set(security, NM_SETTING_WIRELESS_SECURITY_PSK, command.password.c_str(),
                         NM_SETTING_WIRELESS_SECURITY_PSK_FLAGS, NM_SETTING_SECRET_FLAG_NONE,
                         nullptr);
            GVariant *settings = nm_connection_to_dbus(copy, NM_CONNECTION_SERIALIZE_ALL);
            if (settings) {
              settings = g_variant_ref_sink(settings);
              nm_remote_connection_update2(profile, settings, NM_SETTINGS_UPDATE2_FLAG_TO_DISK,
                                           nullptr, nullptr, password_updated, this);
              g_variant_unref(settings);
            } else {
              connecting = false;
              message = "Could not prepare saved profile";
            }
          }
          g_object_unref(copy);
        }
      } else {
        NMConnection *connection = nm_simple_connection_new();
        NMSetting *setting = nm_setting_connection_new();
        g_object_set(setting, NM_SETTING_CONNECTION_ID, command.network.ssid.c_str(),
                     NM_SETTING_CONNECTION_TYPE, NM_SETTING_WIRELESS_SETTING_NAME, nullptr);
        nm_connection_add_setting(connection, setting);
        setting = nm_setting_wireless_new();
        GBytes *ssid = nm_access_point_get_ssid(ap);
        g_object_set(setting, NM_SETTING_WIRELESS_SSID, ssid, nullptr);
        nm_connection_add_setting(connection, setting);
        if (command.network.security == WifiSecurity::Personal) {
          setting = nm_setting_wireless_security_new();
          auto keys = static_cast<unsigned>(nm_access_point_get_wpa_flags(ap)) |
                      static_cast<unsigned>(nm_access_point_get_rsn_flags(ap));
          const bool supported = command.network.uses_sae ?
              (keys & NM_802_11_AP_SEC_KEY_MGMT_SAE) :
              (keys & NM_802_11_AP_SEC_KEY_MGMT_PSK);
          if (!supported) {
            std::fill(command.password.begin(), command.password.end(), '\0');
            g_object_unref(setting);
            g_object_unref(connection);
            connecting = false;
            message = "Network security changed; rescan";
            update();
            return;
          }
          const char *method = command.network.uses_sae ? "sae" : "wpa-psk";
          g_object_set(setting, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, method,
                       NM_SETTING_WIRELESS_SECURITY_PSK, command.password.c_str(), nullptr);
          nm_connection_add_setting(connection, setting);
        }
        nm_client_add_and_activate_connection_async(client, connection, NM_DEVICE(wifi), path,
                                                     nullptr, new_activation_finished, this);
        g_object_unref(connection);
      }
    }
    std::fill(command.password.begin(), command.password.end(), '\0');
    update();
  }

  static gboolean periodic(gpointer data) {
    static_cast<Impl *>(data)->update();
    return G_SOURCE_CONTINUE;
  }

  void run() {
    GMainContext *local = g_main_context_new();
    g_main_context_push_thread_default(local);
    GError *error = nullptr;
    client = nm_client_new(nullptr, &error);
    if (error) {
      message = error->message;
      g_clear_error(&error);
    }
    GSource *timer = g_timeout_source_new(1500);
    g_source_set_callback(timer, periodic, this, nullptr);
    g_source_attach(timer, local);
    g_source_unref(timer);
    {
      std::lock_guard<std::mutex> lock(mutex);
      context = local;
    }
    update();
    for (;;) {
      std::deque<Command> pending;
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (stop) break;
        pending.swap(commands);
      }
      for (auto &command : pending) handle(command);
      g_main_context_iteration(local, TRUE);
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      context = nullptr;
    }
    g_source_destroy(timer);
    g_clear_object(&pending_active);
    g_clear_pointer(&forget_ssid, g_bytes_unref);
    if (client) g_object_unref(client);
    g_main_context_pop_thread_default(local);
    g_main_context_unref(local);
  }
};

WifiBackend::WifiBackend() : impl_(new Impl) {}
WifiBackend::~WifiBackend() = default;
int WifiBackend::fd() const { return impl_->pipe_fd[0]; }
WifiSnapshot WifiBackend::snapshot() {
  char bytes[64];
  while (::read(impl_->pipe_fd[0], bytes, sizeof(bytes)) > 0) {}
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->view;
}
void WifiBackend::scan() { impl_->post({Impl::Action::Scan, {}, {}, false}); }
void WifiBackend::connect(const WifiNetwork &network, std::string password) {
  impl_->post({Impl::Action::Connect, network, std::move(password), false});
}
void WifiBackend::disconnect() { impl_->post({Impl::Action::Disconnect, {}, {}, false}); }
void WifiBackend::forget(const std::string &uuid) {
  WifiNetwork selected;
  selected.saved_uuid = uuid;
  impl_->post({Impl::Action::Forget, std::move(selected), {}, false});
}
void WifiBackend::set_enabled(bool enabled) { impl_->post({Impl::Action::Enable, {}, {}, enabled}); }
}  // namespace cyberdeck
