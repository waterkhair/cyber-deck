#include "audio.hpp"

#include <pulse/pulseaudio.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>

namespace cyberdeck {
namespace {
void configure_pipe(int fd) {
  (void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  (void)fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
}

unsigned volume_percent(const pa_cvolume &volume) {
  return static_cast<unsigned>((static_cast<uint64_t>(pa_cvolume_max(&volume)) * 100 +
                                PA_VOLUME_NORM / 2) / PA_VOLUME_NORM);
}
}  // namespace

struct AudioBackend::Impl {
  int pipe_fd[2] = {-1, -1};
  std::mutex mutex;
  AudioSnapshot view;
  AudioSnapshot pending_view;
  pa_threaded_mainloop *loop = nullptr;
  pa_context *context = nullptr;
  bool started = false;
  bool refreshing = false;
  bool refresh_again = false;
  int remaining = 0;
  std::string default_output, default_input;
  std::string operation_error;
  std::map<std::string, pa_cvolume> output_volumes, input_volumes;

  Impl() {
    if (::pipe(pipe_fd) < 0) {
      view.status = "Audio notification pipe unavailable";
      return;
    }
    configure_pipe(pipe_fd[0]);
    configure_pipe(pipe_fd[1]);
    loop = pa_threaded_mainloop_new();
    if (!loop) { view.status = "Could not start audio client"; return; }
    context = pa_context_new(pa_threaded_mainloop_get_api(loop), "Cyber Deck");
    if (!context) { view.status = "Could not create audio context"; return; }
    pa_context_set_state_callback(context, state_changed, this);
    if (pa_threaded_mainloop_start(loop) < 0) {
      view.status = "Could not start audio event loop";
      return;
    }
    started = true;
    view.status = "Connecting to audio server...";
    pa_threaded_mainloop_lock(loop);
    if (pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0)
      set_unavailable("Audio server unavailable; press r to retry");
    pa_threaded_mainloop_unlock(loop);
  }

  ~Impl() {
    if (started) pa_threaded_mainloop_lock(loop);
    if (context) {
      pa_context_set_state_callback(context, nullptr, nullptr);
      pa_context_set_subscribe_callback(context, nullptr, nullptr);
      if (started) pa_context_disconnect(context);
      pa_context_unref(context);
    }
    if (started) {
      pa_threaded_mainloop_unlock(loop);
      pa_threaded_mainloop_stop(loop);
    }
    if (loop) pa_threaded_mainloop_free(loop);
    if (pipe_fd[0] >= 0) ::close(pipe_fd[0]);
    if (pipe_fd[1] >= 0) ::close(pipe_fd[1]);
  }

  void publish(AudioSnapshot fresh) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      view = std::move(fresh);
    }
    if (pipe_fd[1] >= 0) {
      const char byte = 1;
      (void)::write(pipe_fd[1], &byte, 1);
    }
  }

  void set_unavailable(const char *message) {
    refreshing = false;
    refresh_again = false;
    remaining = 0;
    publish({false, message, {}, {}});
  }

  void reconnect() {
    if (!started) return;
    refreshing = false;
    refresh_again = false;
    remaining = 0;
    operation_error.clear();
    if (context) {
      pa_context_set_state_callback(context, nullptr, nullptr);
      pa_context_set_subscribe_callback(context, nullptr, nullptr);
      pa_context_disconnect(context);
      pa_context_unref(context);
    }
    context = pa_context_new(pa_threaded_mainloop_get_api(loop), "Cyber Deck");
    if (!context) { set_unavailable("Could not create audio context"); return; }
    pa_context_set_state_callback(context, state_changed, this);
    publish({false, "Connecting to audio server...", {}, {}});
    if (pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0)
      set_unavailable("Audio server unavailable; press r to retry");
  }

  static void state_changed(pa_context *ctx, void *data) {
    auto *self = static_cast<Impl *>(data);
    switch (pa_context_get_state(ctx)) {
      case PA_CONTEXT_READY: {
        pa_context_set_subscribe_callback(ctx, subscribed, self);
        pa_operation *operation = pa_context_subscribe(ctx,
            static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SINK |
                PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_SERVER), nullptr, nullptr);
        if (operation) pa_operation_unref(operation);
        self->request_refresh();
        break;
      }
      case PA_CONTEXT_FAILED:
      case PA_CONTEXT_TERMINATED:
        self->set_unavailable("Audio server disconnected; press r to retry");
        break;
      default: break;
    }
  }

  static void subscribed(pa_context *, pa_subscription_event_type_t, uint32_t, void *data) {
    static_cast<Impl *>(data)->request_refresh();
  }

  void finish_part() {
    if (!refreshing || remaining <= 0) return;
    if (--remaining != 0) return;
    refreshing = false;
    if (pending_view.outputs.empty() && pending_view.inputs.empty())
      pending_view.status = "No audio devices found";
    if (!operation_error.empty()) pending_view.status = operation_error;
    for (auto &device : pending_view.outputs) device.is_default = device.name == default_output;
    for (auto &device : pending_view.inputs) device.is_default = device.name == default_input;
    publish(std::move(pending_view));
    if (refresh_again) {
      refresh_again = false;
      request_refresh();
    }
  }

  static void got_server(pa_context *, const pa_server_info *info, void *data) {
    auto *self = static_cast<Impl *>(data);
    if (!self->refreshing) return;
    self->default_output = info && info->default_sink_name ? info->default_sink_name : "";
    self->default_input = info && info->default_source_name ? info->default_source_name : "";
    self->finish_part();
  }

  static void got_output(pa_context *, const pa_sink_info *info, int eol, void *data) {
    auto *self = static_cast<Impl *>(data);
    if (!self->refreshing) return;
    if (eol != 0) { self->finish_part(); return; }
    if (!info || !info->name) return;
    self->output_volumes[info->name] = info->volume;
    self->pending_view.outputs.push_back({info->name,
        info->description ? info->description : info->name,
        volume_percent(info->volume), info->volume.channels, info->mute != 0, false});
  }

  static void got_input(pa_context *, const pa_source_info *info, int eol, void *data) {
    auto *self = static_cast<Impl *>(data);
    if (!self->refreshing) return;
    if (eol != 0) { self->finish_part(); return; }
    if (!info || !info->name || info->monitor_of_sink != PA_INVALID_INDEX) return;
    self->input_volumes[info->name] = info->volume;
    self->pending_view.inputs.push_back({info->name,
        info->description ? info->description : info->name,
        volume_percent(info->volume), info->volume.channels, info->mute != 0, false});
  }

  void request_refresh() {
    if (!context || pa_context_get_state(context) != PA_CONTEXT_READY) return;
    if (refreshing) { refresh_again = true; return; }
    refreshing = true;
    remaining = 3;
    pending_view = {true, "Connected to audio server", {}, {}};
    pa_operation *operation = pa_context_get_server_info(context, got_server, this);
    if (operation) pa_operation_unref(operation); else finish_part();
    operation = pa_context_get_sink_info_list(context, got_output, this);
    if (operation) pa_operation_unref(operation); else finish_part();
    operation = pa_context_get_source_info_list(context, got_input, this);
    if (operation) pa_operation_unref(operation); else finish_part();
  }

  static void changed(pa_context *ctx, int success, void *data) {
    auto *self = static_cast<Impl *>(data);
    self->operation_error = success ? "" : pa_strerror(pa_context_errno(ctx));
    self->request_refresh();
  }

  AudioSnapshot view_copy() {
    std::lock_guard<std::mutex> lock(mutex);
    return view;
  }

  void set_default(bool output, const std::string &name) {
    if (!started) return;
    pa_threaded_mainloop_lock(loop);
    if (context && pa_context_get_state(context) == PA_CONTEXT_READY) {
      pa_operation *operation = output ?
          pa_context_set_default_sink(context, name.c_str(), changed, this) :
          pa_context_set_default_source(context, name.c_str(), changed, this);
      if (operation) pa_operation_unref(operation);
    }
    pa_threaded_mainloop_unlock(loop);
  }

  void set_volume(bool output, const std::string &name, unsigned percent) {
    if (!started) return;
    pa_threaded_mainloop_lock(loop);
    if (context && pa_context_get_state(context) == PA_CONTEXT_READY) {
      const AudioSnapshot current = view_copy();
      const auto &devices = output ? current.outputs : current.inputs;
      auto found = std::find_if(devices.begin(), devices.end(), [&](const AudioDevice &device) {
        return device.name == name;
      });
      auto &volumes = output ? output_volumes : input_volumes;
      auto stored = volumes.find(name);
      if (found != devices.end() && stored != volumes.end()) {
        pa_cvolume volume = stored->second;
        const pa_volume_t target = static_cast<pa_volume_t>(
            static_cast<uint64_t>(PA_VOLUME_NORM) * std::min(percent, 100u) / 100);
        if (pa_cvolume_max(&volume) == PA_VOLUME_MUTED)
          pa_cvolume_set(&volume, found->channels, target);
        else
          pa_cvolume_scale(&volume, target);
        stored->second = volume;
        pa_operation *operation = output ?
            pa_context_set_sink_volume_by_name(context, name.c_str(), &volume, changed, this) :
            pa_context_set_source_volume_by_name(context, name.c_str(), &volume, changed, this);
        if (operation) pa_operation_unref(operation);
      }
    }
    pa_threaded_mainloop_unlock(loop);
  }
};

AudioBackend::AudioBackend() : impl_(new Impl) {}
AudioBackend::~AudioBackend() = default;
int AudioBackend::fd() const { return impl_->pipe_fd[0]; }
AudioSnapshot AudioBackend::snapshot() {
  if (impl_->pipe_fd[0] >= 0) {
    char bytes[64];
    while (::read(impl_->pipe_fd[0], bytes, sizeof(bytes)) > 0) {}
  }
  return impl_->view_copy();
}
void AudioBackend::refresh() {
  if (!impl_->started) return;
  pa_threaded_mainloop_lock(impl_->loop);
  const pa_context_state_t state = impl_->context ? pa_context_get_state(impl_->context) :
      PA_CONTEXT_UNCONNECTED;
  if (state == PA_CONTEXT_READY) impl_->request_refresh();
  else if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED ||
           state == PA_CONTEXT_UNCONNECTED) impl_->reconnect();
  pa_threaded_mainloop_unlock(impl_->loop);
}
void AudioBackend::set_default(bool output, const std::string &name) {
  impl_->set_default(output, name);
}
void AudioBackend::set_volume(bool output, const std::string &name, unsigned percent) {
  impl_->set_volume(output, name, percent);
}
}  // namespace cyberdeck
