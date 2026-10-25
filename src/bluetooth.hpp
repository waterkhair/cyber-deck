#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cyberdeck {

struct BluetoothDevice {
  std::string path;
  std::string name;
  std::string address;
  bool paired = false;
  bool trusted = false;
  bool connected = false;
  int signal = 0;
};

enum class BluetoothPromptKind { None, Confirm, Pin, Passkey };

struct BluetoothSnapshot {
  bool available = false;
  bool powered = false;
  bool discovering = false;
  bool busy = false;
  std::string status = "BlueZ unavailable";
  std::vector<BluetoothDevice> devices;
  BluetoothPromptKind prompt = BluetoothPromptKind::None;
  uint64_t prompt_epoch = 0;
  std::string prompt_device;
  unsigned prompt_passkey = 0;
  bool prompt_has_passkey = false;
};

class BluetoothBackend {
 public:
  BluetoothBackend();
  ~BluetoothBackend();
  BluetoothBackend(const BluetoothBackend &) = delete;
  BluetoothBackend &operator=(const BluetoothBackend &) = delete;

  int fd() const;
  BluetoothSnapshot snapshot();
  void refresh();
  void set_powered(bool powered);
  void set_discovering(bool discovering);
  void pair(const std::string &path, bool connect_after = false);
  void connect(const std::string &path);
  void disconnect(const std::string &path);
  void forget(const std::string &path);
  void set_trusted(const std::string &path, bool trusted);
  void answer_prompt(uint64_t epoch, bool accepted, std::string code = {});

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cyberdeck
