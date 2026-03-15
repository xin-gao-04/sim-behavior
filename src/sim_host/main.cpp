#include <csignal>
#include <cstdlib>
#include <iostream>

#include "sim_host_app.hpp"

static sim_bt::SimHostApp* g_app = nullptr;

static void OnSignal(int /*signum*/) {
  std::cout << "\n[sim_host] Received stop signal, shutting down...\n";
  if (g_app) g_app->RequestStop();
}

int main(int /*argc*/, char** /*argv*/) {
  sim_bt::SimHostApp app;
  g_app = &app;

  std::signal(SIGINT,  OnSignal);
  std::signal(SIGTERM, OnSignal);

  auto status = app.Initialize();
  if (!status) {
    std::cerr << "[sim_host] Initialize failed: " << status.message << "\n";
    return EXIT_FAILURE;
  }

  std::cout << "[sim_host] Running. Press Ctrl+C to stop.\n";
  app.Run();
  std::cout << "[sim_host] Stopped.\n";
  return EXIT_SUCCESS;
}
