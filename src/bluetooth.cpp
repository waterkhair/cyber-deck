#include "bluetooth.hpp"

#include <gio/gio.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace cyberdeck {
namespace {
constexpr const char *kBluez = "org.bluez";
constexpr const char *kAgentPath = "/org/cyberdeck/agent";
constexpr const char *kAgentXml =
    "<node><interface name='org.bluez.Agent1'>"
    "<method name='Release'/>"
    "<method name='RequestPinCode'><arg type='o' direction='in'/>"
    "<arg type='s' direction='out'/></method>"
    "<method name='DisplayPinCode'><arg type='o' direction='in'/>"
    "<arg type='s' direction='in'/></method>"
    "<method name='RequestPasskey'><arg type='o' direction='in'/>"
    "<arg type='u' direction='out'/></method>"
    "<method name='DisplayPasskey'><arg type='o' direction='in'/>"
    "<arg type='u' direction='in'/><arg type='q' direction='in'/></method>"
    "<method name='RequestConfirmation'><arg type='o' direction='in'/>"
    "<arg type='u' direction='in'/></method>"
    "<method name='RequestAuthorization'><arg type='o' direction='in'/></method>"
    "<method name='AuthorizeService'><arg type='o' direction='in'/>"
    "<arg type='s' direction='in'/></method>"
    "<method name='Cancel'/></interface></node>";

void configure_pipe(int fd) {
  (void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  (void)fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
}

std::string string_property(GVariant *properties, const char *key) {
  GVariant *value = g_variant_lookup_value(properties, key, G_VARIANT_TYPE_STRING);
  if (!value) return {};
  std::string text = g_variant_get_string(value, nullptr);
  g_variant_unref(value);
  for (char &ch : text)
    if (static_cast<unsigned char>(ch) < 32 || ch == 127) ch = '?';
  return text;
}

bool bool_property(GVariant *properties, const char *key) {
  gboolean result = FALSE;
  (void)g_variant_lookup(properties, key, "b", &result);
  return result != FALSE;
}

int signal_property(GVariant *properties) {
  GVariant *value = g_variant_lookup_value(properties, "RSSI", G_VARIANT_TYPE_INT16);
  if (!value) return 0;
  int result = g_variant_get_int16(value);
  g_variant_unref(value);
  return result;
}

std::string error_text(GError *error) {
  if (!error) return "unknown error";
  std::string result = error->message ? error->message : "unknown error";
  g_error_free(error);
  return result;
}
}  // namespace

struct BluetoothBackend::Impl {
  enum class Action { Refresh, Power, Discover, Pair, Connect, Disconnect, Forget, Trust, Answer };
  struct Command {
    Action action;
    std::string path;
    bool value = false;
    uint64_t epoch = 0;
    std::string code;
  };
  struct Call {
    Impl *self;
    Action action;
    std::string path;
    bool connect_after;
  };

  int pipe_fd[2] = {-1, -1};
  GMainContext *context = nullptr;
  GMainLoop *loop = nullptr;
  GDBusConnection *bus = nullptr;
  GCancellable *cancellable = nullptr;
  GDBusNodeInfo *agent_info = nullptr;
  GDBusMethodInvocation *prompt_invocation = nullptr;
  guint agent_id = 0;
  guint added_signal = 0;
  guint removed_signal = 0;
  guint changed_signal = 0;
  bool agent_registered = false;
  bool refresh_queued = false;
  uint64_t scan_generation = 0;
  bool busy = false;
  std::atomic<bool> stopping{false};
  int pending_calls = 0;
  std::string adapter_path;
  std::string status = "Connecting to BlueZ...";
  std::vector<BluetoothDevice> devices;
  BluetoothPromptKind prompt = BluetoothPromptKind::None;
  uint64_t prompt_epoch = 0;
  std::string prompt_device;
  unsigned prompt_passkey = 0;
  bool prompt_has_passkey = false;
  BluetoothSnapshot state;
  std::mutex state_mutex;
  std::mutex command_mutex;
  std::deque<Command> commands;
  std::thread worker;

  Impl() {
    if (::pipe(pipe_fd) < 0) {
      status = "Bluetooth notification pipe unavailable";
      state.status = status;
      return;
    }
    configure_pipe(pipe_fd[0]);
    configure_pipe(pipe_fd[1]);
    context = g_main_context_new();
    cancellable = g_cancellable_new();
    state.status = status;
    worker = std::thread([this] { run(); });
  }

  ~Impl() {
    {
      std::lock_guard<std::mutex> lock(command_mutex);
      stopping = true;
    }
    if (cancellable) g_cancellable_cancel(cancellable);
    if (context) post([](gpointer data) -> gboolean {
      auto *self = static_cast<Impl *>(data);
      if (self->loop) g_main_loop_quit(self->loop);
      return G_SOURCE_REMOVE;
    });
    if (worker.joinable()) worker.join();
    if (cancellable) g_object_unref(cancellable);
    if (context) g_main_context_unref(context);
    if (pipe_fd[0] >= 0) ::close(pipe_fd[0]);
    if (pipe_fd[1] >= 0) ::close(pipe_fd[1]);
  }

  void post(GSourceFunc callback) {
    GSource *source = g_idle_source_new();
    g_source_set_callback(source, callback, this, nullptr);
    g_source_attach(source, context);
    g_source_unref(source);
    g_main_context_wakeup(context);
  }

  void notify(BluetoothSnapshot next) {
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      state = std::move(next);
    }
    const char byte = 1;
    (void)::write(pipe_fd[1], &byte, 1);
  }

  void publish() {
    BluetoothSnapshot next;
    next.available = !adapter_path.empty();
    next.powered = state_powered;
    next.discovering = state_discovering;
    next.busy = busy;
    next.status = status;
    next.devices = devices;
    next.prompt = prompt;
    next.prompt_epoch = prompt_epoch;
    next.prompt_device = prompt_device;
    next.prompt_passkey = prompt_passkey;
    next.prompt_has_passkey = prompt_has_passkey;
    notify(std::move(next));
  }

  bool state_powered = false;
  bool state_discovering = false;

  BluetoothSnapshot snapshot() {
    char data[128];
    while (pipe_fd[0] >= 0 && ::read(pipe_fd[0], data, sizeof(data)) > 0) {}
    std::lock_guard<std::mutex> lock(state_mutex);
    return state;
  }

  void enqueue(Command command) {
    if (!context) return;
    {
      std::lock_guard<std::mutex> lock(command_mutex);
      if (stopping) return;
      commands.push_back(std::move(command));
    }
    post([](gpointer data) -> gboolean {
      static_cast<Impl *>(data)->drain_commands();
      return G_SOURCE_REMOVE;
    });
  }

  static void on_change(GDBusConnection *, const gchar *, const gchar *,
                        const gchar *, const gchar *, GVariant *, gpointer data) {
    static_cast<Impl *>(data)->schedule_refresh();
  }

  void schedule_refresh() {
    if (refresh_queued || stopping) return;
    refresh_queued = true;
    GSource *source = g_timeout_source_new(120);
    g_source_set_callback(source, [](gpointer data) -> gboolean {
      auto *self = static_cast<Impl *>(data);
      self->refresh_queued = false;
      self->refresh_now();
      return G_SOURCE_REMOVE;
    }, this, nullptr);
    g_source_attach(source, context);
    g_source_unref(source);
  }

  void refresh_now() {
    if (!bus || stopping) return;
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_sync(
        bus, kBluez, "/", "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects", nullptr, G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE, 2500, cancellable, &error);
    if (!reply) {
      adapter_path.clear();
      devices.clear();
      state_powered = false;
      state_discovering = false;
      status = "BlueZ unavailable: " + error_text(error);
      publish();
      return;
    }
    GVariant *objects = nullptr;
    g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);
    GVariantIter iter;
    g_variant_iter_init(&iter, objects);
    const gchar *path = nullptr;
    GVariant *interfaces = nullptr;
    std::string new_adapter;
    bool powered = false;
    bool discovering = false;
    std::vector<BluetoothDevice> found;
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
      GVariant *adapter = g_variant_lookup_value(
          interfaces, "org.bluez.Adapter1", G_VARIANT_TYPE("a{sv}"));
      if (adapter && (new_adapter.empty() || adapter_path == path)) {
        new_adapter = path;
        powered = bool_property(adapter, "Powered");
        discovering = bool_property(adapter, "Discovering");
      }
      if (adapter) g_variant_unref(adapter);
      GVariant *device = g_variant_lookup_value(
          interfaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"));
      if (device) {
        BluetoothDevice item;
        item.path = path;
        item.name = string_property(device, "Alias");
        if (item.name.empty()) item.name = string_property(device, "Name");
        item.address = string_property(device, "Address");
        if (item.name.empty()) item.name = item.address;
        item.paired = bool_property(device, "Paired");
        item.trusted = bool_property(device, "Trusted");
        item.connected = bool_property(device, "Connected");
        item.signal = signal_property(device);
        found.push_back(std::move(item));
        g_variant_unref(device);
      }
      g_variant_unref(interfaces);
    }
    g_variant_unref(objects);
    g_variant_unref(reply);
    adapter_path = std::move(new_adapter);
    state_powered = powered;
    state_discovering = discovering;
    found.erase(std::remove_if(found.begin(), found.end(), [this](const BluetoothDevice &device) {
      return adapter_path.empty() ||
             device.path.compare(0, adapter_path.size() + 1, adapter_path + "/") != 0;
    }), found.end());
    std::sort(found.begin(), found.end(), [](const BluetoothDevice &a, const BluetoothDevice &b) {
      if (a.connected != b.connected) return a.connected > b.connected;
      if (a.paired != b.paired) return a.paired > b.paired;
      return a.name < b.name;
    });
    if (found.size() > 100) found.resize(100);
    devices = std::move(found);
    if (adapter_path.empty()) status = "No Bluetooth adapter found";
    else if (status == "Connecting to BlueZ..." || status == "No Bluetooth adapter found" ||
             status.rfind("BlueZ unavailable:", 0) == 0)
      status = "Adapter available";
    publish();
  }

  std::string device_name(const std::string &path) const {
    auto it = std::find_if(devices.begin(), devices.end(),
        [&path](const BluetoothDevice &device) { return device.path == path; });
    return it == devices.end() ? "device" : it->name;
  }

  static void on_agent_call(GDBusConnection *, const gchar *, const gchar *,
                            const gchar *, const gchar *method, GVariant *params,
                            GDBusMethodInvocation *invocation, gpointer data) {
    static_cast<Impl *>(data)->agent_call(method, params, invocation);
  }

  void clear_prompt() {
    prompt_invocation = nullptr;
    prompt = BluetoothPromptKind::None;
    prompt_device.clear();
    prompt_passkey = 0;
    prompt_has_passkey = false;
    publish();
  }

  void agent_call(const char *method, GVariant *params, GDBusMethodInvocation *invocation) {
    if (g_strcmp0(method, "Release") == 0) {
      if (prompt_invocation) {
        g_dbus_method_invocation_return_dbus_error(
            prompt_invocation, "org.bluez.Error.Canceled", "Pairing agent released");
        clear_prompt();
      }
      g_dbus_method_invocation_return_value(invocation, nullptr);
      agent_registered = false;
      return;
    }
    if (g_strcmp0(method, "Cancel") == 0) {
      if (prompt_invocation) {
        g_dbus_method_invocation_return_dbus_error(
            prompt_invocation, "org.bluez.Error.Canceled", "Pairing canceled");
        clear_prompt();
      }
      g_dbus_method_invocation_return_value(invocation, nullptr);
      return;
    }
    if (g_strcmp0(method, "DisplayPinCode") == 0) {
      const gchar *path, *pin;
      g_variant_get(params, "(&o&s)", &path, &pin);
      status = "Enter PIN " + std::string(pin) + " on " + device_name(path);
      publish();
      g_dbus_method_invocation_return_value(invocation, nullptr);
      return;
    }
    if (g_strcmp0(method, "DisplayPasskey") == 0) {
      const gchar *path;
      guint32 passkey;
      guint16 entered;
      g_variant_get(params, "(&ouq)", &path, &passkey, &entered);
      (void)entered;
      char code[7];
      std::snprintf(code, sizeof(code), "%06u", passkey);
      status = "Enter code " + std::string(code) + " on " + device_name(path);
      publish();
      g_dbus_method_invocation_return_value(invocation, nullptr);
      return;
    }
    if (prompt_invocation) {
      g_dbus_method_invocation_return_dbus_error(
          invocation, "org.bluez.Error.Rejected", "Another pairing prompt is active");
      return;
    }
    const gchar *path = nullptr;
    if (g_strcmp0(method, "RequestConfirmation") == 0) {
      guint32 code;
      g_variant_get(params, "(&ou)", &path, &code);
      prompt = BluetoothPromptKind::Confirm;
      prompt_passkey = code;
      prompt_has_passkey = true;
    } else if (g_strcmp0(method, "RequestPinCode") == 0) {
      g_variant_get(params, "(&o)", &path);
      prompt = BluetoothPromptKind::Pin;
    } else if (g_strcmp0(method, "RequestPasskey") == 0) {
      g_variant_get(params, "(&o)", &path);
      prompt = BluetoothPromptKind::Passkey;
    } else if (g_strcmp0(method, "RequestAuthorization") == 0) {
      g_variant_get(params, "(&o)", &path);
      prompt = BluetoothPromptKind::Confirm;
    } else if (g_strcmp0(method, "AuthorizeService") == 0) {
      const gchar *service;
      g_variant_get(params, "(&o&s)", &path, &service);
      prompt = BluetoothPromptKind::Confirm;
    } else {
      g_dbus_method_invocation_return_dbus_error(
          invocation, "org.bluez.Error.Rejected", "Unsupported pairing request");
      return;
    }
    prompt_invocation = invocation;
    prompt_device = device_name(path);
    ++prompt_epoch;
    status = "Pairing confirmation needed";
    publish();
  }

  void answer(const Command &command) {
    if (!prompt_invocation || command.epoch != prompt_epoch) return;
    auto *invocation = prompt_invocation;
    if (!command.value) {
      g_dbus_method_invocation_return_dbus_error(
          invocation, "org.bluez.Error.Rejected", "Pairing declined");
    } else if (prompt == BluetoothPromptKind::Pin) {
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", command.code.c_str()));
    } else if (prompt == BluetoothPromptKind::Passkey) {
      const bool numeric = !command.code.empty() && command.code.size() <= 6 &&
          std::all_of(command.code.begin(), command.code.end(),
              [](unsigned char ch) { return ch >= '0' && ch <= '9'; });
      if (!numeric) {
        g_dbus_method_invocation_return_dbus_error(
            invocation, "org.bluez.Error.Rejected", "Invalid passkey");
      } else {
        guint32 code = static_cast<guint32>(std::stoul(command.code));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", code));
      }
    } else {
      g_dbus_method_invocation_return_value(invocation, nullptr);
    }
    status = command.value ? "Pairing..." : "Pairing declined";
    clear_prompt();
  }

  static void call_finished(GObject *source, GAsyncResult *result, gpointer data) {
    std::unique_ptr<Call> call(static_cast<Call *>(data));
    Impl *self = call->self;
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_finish(
        G_DBUS_CONNECTION(source), result, &error);
    const bool succeeded = reply != nullptr;
    --self->pending_calls;
    self->busy = false;
    if (reply) {
      g_variant_unref(reply);
      if (call->action == Action::Connect)
        self->status = "Connected to " + self->device_name(call->path);
      else if (call->action == Action::Pair)
        self->status = "Paired with " + self->device_name(call->path);
      else self->status = "Done";
    } else if (!self->stopping) {
      self->status = "Bluetooth: " + error_text(error);
      error = nullptr;
    }
    if (error) g_error_free(error);
    if (!self->stopping) {
      self->refresh_now();
      if (succeeded && call->action == Action::Pair && call->connect_after) {
        const auto paired = std::find_if(self->devices.begin(), self->devices.end(),
            [&](const BluetoothDevice &device) { return device.path == call->path; });
        if (paired != self->devices.end() && !paired->connected) {
          std::lock_guard<std::mutex> lock(self->command_mutex);
          self->commands.push_front({Action::Connect, call->path, false, 0, {}});
        }
      }
      self->publish();
      self->drain_commands();
    }
  }

  void call(Action action, const std::string &path, const char *interface,
            const char *method, GVariant *parameters, bool connect_after = false) {
    busy = true;
    if (action == Action::Connect) status = "Connecting to " + device_name(path) + "...";
    else if (action == Action::Pair) status = "Pairing with " + device_name(path) + "...";
    else status = "Working...";
    publish();
    ++pending_calls;
    g_dbus_connection_call(bus, kBluez, path.c_str(), interface, method,
        parameters, nullptr, G_DBUS_CALL_FLAGS_NONE,
        action == Action::Pair ? 120000 : 20000, cancellable, call_finished,
        new Call{this, action, path, connect_after});
  }

  void dispatch_command(const Command &command) {
    if (command.action == Action::Answer) { answer(command); return; }
    if (command.action == Action::Refresh) { refresh_now(); return; }
    if (adapter_path.empty()) {
      status = "No Bluetooth adapter found";
      publish();
      return;
    }
    if (command.action == Action::Power) {
      call(command.action, adapter_path, "org.freedesktop.DBus.Properties", "Set",
           g_variant_new("(ssv)", "org.bluez.Adapter1", "Powered",
                         g_variant_new_boolean(command.value)));
    } else if (command.action == Action::Discover) {
      const uint64_t generation = ++scan_generation;
      if (command.value) {
        auto *timeout = new std::pair<Impl *, uint64_t>(this, generation);
        GSource *source = g_timeout_source_new(20000);
        g_source_set_callback(source, [](gpointer data) -> gboolean {
          const auto &timeout = *static_cast<std::pair<Impl *, uint64_t> *>(data);
          if (!timeout.first->stopping && timeout.first->scan_generation == timeout.second &&
              timeout.first->state_discovering)
            timeout.first->enqueue({Action::Discover, {}, false, 0, {}});
          return G_SOURCE_REMOVE;
        }, timeout, [](gpointer data) {
          delete static_cast<std::pair<Impl *, uint64_t> *>(data);
        });
        g_source_attach(source, context);
        g_source_unref(source);
      }
      call(command.action, adapter_path, "org.bluez.Adapter1",
           command.value ? "StartDiscovery" : "StopDiscovery", nullptr);
    } else if (command.action == Action::Pair) {
      call(command.action, command.path, "org.bluez.Device1", "Pair", nullptr, command.value);
    } else if (command.action == Action::Connect) {
      call(command.action, command.path, "org.bluez.Device1", "Connect", nullptr);
    } else if (command.action == Action::Disconnect) {
      call(command.action, command.path, "org.bluez.Device1", "Disconnect", nullptr);
    } else if (command.action == Action::Forget) {
      call(command.action, adapter_path, "org.bluez.Adapter1", "RemoveDevice",
           g_variant_new("(o)", command.path.c_str()));
    } else if (command.action == Action::Trust) {
      call(command.action, command.path, "org.freedesktop.DBus.Properties", "Set",
           g_variant_new("(ssv)", "org.bluez.Device1", "Trusted",
                         g_variant_new_boolean(command.value)));
    }
  }

  void drain_commands() {
    std::deque<Command> incoming, deferred;
    {
      std::lock_guard<std::mutex> lock(command_mutex);
      incoming.swap(commands);
    }
    for (auto &command : incoming) {
      if (busy && command.action != Action::Answer && command.action != Action::Refresh)
        deferred.push_back(std::move(command));
      else dispatch_command(command);
    }
    if (!deferred.empty()) {
      std::lock_guard<std::mutex> lock(command_mutex);
      while (!deferred.empty()) {
        commands.push_front(std::move(deferred.back()));
        deferred.pop_back();
      }
    }
  }

  void run() {
    g_main_context_push_thread_default(context);
    loop = g_main_loop_new(context, FALSE);
    GError *error = nullptr;
    bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, cancellable, &error);
    if (!bus) {
      status = "System D-Bus unavailable: " + error_text(error);
      publish();
    } else {
      added_signal = g_dbus_connection_signal_subscribe(bus, kBluez,
          "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", nullptr, nullptr,
          G_DBUS_SIGNAL_FLAGS_NONE, on_change, this, nullptr);
      removed_signal = g_dbus_connection_signal_subscribe(bus, kBluez,
          "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", nullptr, nullptr,
          G_DBUS_SIGNAL_FLAGS_NONE, on_change, this, nullptr);
      changed_signal = g_dbus_connection_signal_subscribe(bus, kBluez,
          "org.freedesktop.DBus.Properties", "PropertiesChanged", nullptr, nullptr,
          G_DBUS_SIGNAL_FLAGS_NONE, on_change, this, nullptr);
      agent_info = g_dbus_node_info_new_for_xml(kAgentXml, &error);
      if (agent_info) {
        static GDBusInterfaceVTable vtable{};
        vtable.method_call = on_agent_call;
        agent_id = g_dbus_connection_register_object(
            bus, kAgentPath, agent_info->interfaces[0], &vtable, this, nullptr, &error);
        if (agent_id) {
          GVariant *reply = g_dbus_connection_call_sync(
              bus, kBluez, "/org/bluez", "org.bluez.AgentManager1", "RegisterAgent",
              g_variant_new("(os)", kAgentPath, "KeyboardDisplay"), nullptr,
              G_DBUS_CALL_FLAGS_NONE, 5000, cancellable, &error);
          if (reply) { agent_registered = true; g_variant_unref(reply); }
        }
      }
      if (error) { g_error_free(error); error = nullptr; }
      refresh_now();
    }
    if (!stopping) g_main_loop_run(loop);
    if (prompt_invocation) {
      g_dbus_method_invocation_return_dbus_error(
          prompt_invocation, "org.bluez.Error.Canceled", "Cyber Deck is closing");
      prompt_invocation = nullptr;
    }
    while (pending_calls > 0) g_main_context_iteration(context, TRUE);
    if (bus) {
      if (added_signal) g_dbus_connection_signal_unsubscribe(bus, added_signal);
      if (removed_signal) g_dbus_connection_signal_unsubscribe(bus, removed_signal);
      if (changed_signal) g_dbus_connection_signal_unsubscribe(bus, changed_signal);
      if (agent_id) g_dbus_connection_unregister_object(bus, agent_id);
      g_object_unref(bus);
    }
    if (agent_info) g_dbus_node_info_unref(agent_info);
    g_main_loop_unref(loop);
    loop = nullptr;
    g_main_context_pop_thread_default(context);
  }
};

BluetoothBackend::BluetoothBackend() : impl_(std::make_unique<Impl>()) {}
BluetoothBackend::~BluetoothBackend() = default;
int BluetoothBackend::fd() const { return impl_->pipe_fd[0]; }
BluetoothSnapshot BluetoothBackend::snapshot() { return impl_->snapshot(); }
void BluetoothBackend::refresh() { impl_->enqueue({Impl::Action::Refresh, {}, false, 0, {}}); }
void BluetoothBackend::set_powered(bool value) {
  impl_->enqueue({Impl::Action::Power, {}, value, 0, {}});
}
void BluetoothBackend::set_discovering(bool value) {
  impl_->enqueue({Impl::Action::Discover, {}, value, 0, {}});
}
void BluetoothBackend::pair(const std::string &path, bool connect_after) {
  impl_->enqueue({Impl::Action::Pair, path, connect_after, 0, {}});
}
void BluetoothBackend::connect(const std::string &path) {
  impl_->enqueue({Impl::Action::Connect, path, false, 0, {}});
}
void BluetoothBackend::disconnect(const std::string &path) {
  impl_->enqueue({Impl::Action::Disconnect, path, false, 0, {}});
}
void BluetoothBackend::forget(const std::string &path) {
  impl_->enqueue({Impl::Action::Forget, path, false, 0, {}});
}
void BluetoothBackend::set_trusted(const std::string &path, bool trusted) {
  impl_->enqueue({Impl::Action::Trust, path, trusted, 0, {}});
}
void BluetoothBackend::answer_prompt(uint64_t epoch, bool accepted, std::string code) {
  impl_->enqueue({Impl::Action::Answer, {}, accepted, epoch, std::move(code)});
}

}  // namespace cyberdeck
