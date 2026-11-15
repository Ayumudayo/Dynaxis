from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy
import os


class ProjectConan(ConanFile):
    name = "dynaxis"
    version = "0.1.0"
    package_type = "application"
    required_conan_version = "==2.29.1"

    settings = "os", "compiler", "build_type", "arch"
    options = {
        "build_profile": ["windows-dev", "windows-client", "server", "core"],
    }
    default_options = {
        "build_profile": "windows-dev",
        "*:shared": False,
    }

    def layout(self):
        cmake_layout(self)

    def requirements(self):
        feature = str(self.options.build_profile)

        self.requires("boost/1.90.0")
        if feature == "core":
            self.requires("gtest/1.17.0")
            return
        self.requires("protobuf/6.32.1")
        self.requires("openssl/3.6.1")
        self.requires("lz4/1.9.4")

        if feature == "windows-client":
            self.requires("glfw/3.4")
            self.requires("opengl/system")
            return

        self.requires("libpqxx/7.10.5")
        self.requires("redis-plus-plus/1.3.15")
        self.requires("gtest/1.17.0")
        self.requires("nlohmann_json/3.12.0")

        if feature == "windows-dev":
            self.requires("glfw/3.4")
            self.requires("opengl/system")

    def generate(self):
        deps = CMakeDeps(self)
        deps.set_property("boost", "cmake_file_name", "Boost")
        deps.set_property("protobuf", "cmake_file_name", "Protobuf")
        deps.set_property("glfw", "cmake_file_name", "glfw3")
        deps.set_property("glfw", "cmake_target_name", "glfw")
        deps.set_property("gtest", "cmake_file_name", "GTest")
        deps.set_property("redis-plus-plus", "cmake_file_name", "redis++")
        deps.set_property("redis-plus-plus", "cmake_target_name", "redis++::redis++")
        deps.generate()

        toolchain = CMakeToolchain(self)
        toolchain.user_presets_path = ""
        toolchain.generate()

        # Runtime images carry Conan-built shared dependencies, never distro substitutes.
        runtime_dir = os.path.join(self.build_folder, "runtime-libs")
        os.makedirs(runtime_dir, exist_ok=True)
        for dependency in self.dependencies.host.values():
            for libdir in dependency.cpp_info.libdirs:
                copy(self, "*.so*", libdir, runtime_dir, keep_path=False)
                copy(self, "*.dylib*", libdir, runtime_dir, keep_path=False)
