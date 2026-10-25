#include "application.hpp"

int main(int argc, char **argv) {
  try {
    std::string shell = cyberdeck::select_shell(argc, argv);
    if (shell.empty()) return 0;
    std::setlocale(LC_ALL, "");
    cyberdeck::CursesHost host;
    cyberdeck::Application app(shell, host.default_colors());
    app.run();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "cyber-deck: " << error.what() << '\n';
    return 1;
  }
}
