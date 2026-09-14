from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.build import check_min_cppstd
from conan.tools.files import copy
import os


class ThunderboltConan(ConanFile):
    name = "thunderbolt"
    version = "0.1.0"
    license = "Apache-2.0"
    url = "https://github.com/ankitlinkesh/ThunderBolt-HT"
    homepage = "https://github.com/ankitlinkesh/ThunderBolt-HT"
    description = "A high-performance parallel CPU execution runtime"
    topics = ("concurrency", "task-scheduling", "work-stealing", "parallelism")

    settings = "os", "compiler", "build_type", "arch"

    # thunderbolt/ is the standalone-buildable unit (S4) - this recipe packages
    # exactly that directory, not the repo's engine/ or game_benchmarks/, which
    # are the simulation that exercises the runtime, not the runtime itself.
    #
    # Exported explicitly (not via the exports_sources attribute) because
    # thunderbolt/CMakeLists.txt reaches its own public headers through the
    # directory CONTAINING thunderbolt/ - every #include in this codebase reads
    # <thunderbolt/api/...>. Exporting api/, core/, etc. flat relative to this
    # conanfile.py (which lives inside thunderbolt/) loses that wrapping folder
    # and breaks every include; copying them back under an explicit thunderbolt/
    # subfolder keeps the same layout the real repo has.
    def export_sources(self):
        dest = os.path.join(self.export_sources_folder, "thunderbolt")
        copy(self, "CMakeLists.txt", src=self.recipe_folder, dst=dest)
        for sub in ("cmake", "api", "core", "cpu", "runtime"):
            copy(self, "*", src=os.path.join(self.recipe_folder, sub),
                 dst=os.path.join(dest, sub))

    def layout(self):
        cmake_layout(self)
        self.folders.source = "thunderbolt"

    def validate(self):
        check_min_cppstd(self, 20)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["THUNDERBOLT_BUILD_TESTS"] = False
        tc.variables["THUNDERBOLT_BUILD_EXAMPLES"] = False
        tc.variables["THUNDERBOLT_BUILD_BENCHMARKS"] = False
        tc.variables["THUNDERBOLT_INSTALL"] = True
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build(target="thunderbolt")

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        # Tried pointing Conan's CMakeDeps generator at thunderbolt's own
        # installed config (cmake_find_mode="config") first, on the reasoning
        # that we already have a correct one and generating a second, parallel
        # one seemed redundant. Tested it directly: configure succeeded and
        # declared the target, but linking failed with unresolved externals -
        # the imported target Conan produced that way did not carry the real
        # .lib location. Reverted to the standard cpp_info-described package,
        # which linked correctly on the same test in the same run.
        self.cpp_info.set_property("cmake_file_name", "Thunderbolt")
        self.cpp_info.set_property("cmake_target_name", "thunderbolt::thunderbolt")
        self.cpp_info.libs = ["thunderbolt"]
        if self.settings.os == "Windows":
            self.cpp_info.system_libs = ["kernel32"]
        else:
            self.cpp_info.system_libs = ["pthread"]
