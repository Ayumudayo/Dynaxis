#include "server/core/plugin/shared_library.hpp"

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace server::core::plugin {

SharedLibrary::~SharedLibrary() {
    close();
}

SharedLibrary::SharedLibrary(SharedLibrary&& other) noexcept
    : handle_(other.handle_) {
    other.handle_ = nullptr;
}

SharedLibrary& SharedLibrary::operator=(SharedLibrary&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    close();
    handle_ = other.handle_;
    other.handle_ = nullptr;
    return *this;
}

bool SharedLibrary::open(const std::filesystem::path& path, std::string& error) {
    close();
    error.clear();

#if defined(_WIN32)
    HMODULE mod = ::LoadLibraryW(path.wstring().c_str());
    if (!mod) {
        error = "LoadLibrary failed";
        return false;
    }
    handle_ = reinterpret_cast<void*>(mod);
#else
    (void)::dlerror();
    void* mod = ::dlopen(path.c_str(), RTLD_NOW);
    if (!mod) {
        const char* msg = ::dlerror();
        error = msg ? msg : "dlopen failed";
        return false;
    }
    handle_ = mod;
#endif

    return true;
}

void SharedLibrary::close() {
    if (!handle_) {
        return;
    }

#if defined(_WIN32)
    (void)::FreeLibrary(reinterpret_cast<HMODULE>(handle_));
#else
    (void)::dlclose(handle_);
#endif

    handle_ = nullptr;
}

void* SharedLibrary::symbol(const char* name, std::string& error) const {
    error.clear();

    if (!name || !*name) {
        error = "symbol name is empty";
        return nullptr;
    }

    if (!handle_) {
        error = "library not loaded";
        return nullptr;
    }

#if defined(_WIN32)
    FARPROC addr = ::GetProcAddress(reinterpret_cast<HMODULE>(handle_), name);
    if (!addr) {
        error = "GetProcAddress failed";
        return nullptr;
    }
    return reinterpret_cast<void*>(addr);
#else
    (void)::dlerror();
    void* addr = ::dlsym(handle_, name);
    const char* msg = ::dlerror();
    if (msg != nullptr) {
        error = msg;
        return nullptr;
    }
    return addr;
#endif
}

bool SharedLibrary::is_loaded() const {
    return handle_ != nullptr;
}

} // namespace server::core::plugin
