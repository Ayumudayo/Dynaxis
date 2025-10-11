#pragma once

#include <memory>

class NetClient;

namespace client::app {

class AppState;
class CommandProcessor;
class NetworkRouter;
class UiBuilder;

class Application {
public:
    Application();
    ~Application();

    int Run();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace client::app

