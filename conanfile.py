from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain, cmake_layout


class ProjectConan(ConanFile):
    name = "dynaxis"
    version = "0.1.0"
    package_type = "application"

    settings = "os", "compiler", "build_type", "arch"
    options = {
        "build_profile": ["windows-dev", "windows-client"],
    }
    default_options = {
        "build_profile": "windows-dev",
        "*:shared": False,
    }

    def layout(self):
        cmake_layout(self)

    def requirements(self):
        feature = str(self.options.build_profile)

        self.requires("boost/[>=1.83 <2]")
        self.requires("protobuf/[>=3.21 <7]")
        self.requires("openssl/[>=3 <4]")
        self.requires("lz4/1.9.4")

        if feature == "windows-client":
            self.requires("glfw/[>=3.3 <4]")
            self.requires("opengl/system")
            return

        self.requires("libpqxx/[>=7 <8]")
        self.requires("redis-plus-plus/[>=1.3 <2]")
        self.requires("gtest/[>=1.14 <2]")
        self.requires("nlohmann_json/[>=3.11 <4]")

        if str(self.settings.os) == "Windows":
            self.requires("glfw/[>=3.3 <4]")
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
