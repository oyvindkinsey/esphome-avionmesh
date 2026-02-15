"""Pre-build script: add library INCLUDE_DIRS to CMakeLists.txt."""
Import("env")

import os

project_dir = env.subst("$PROJECT_DIR")

# Find avionmesh library dir (recsrmesh is bundled inside it as a submodule)
# PlatformIO uses .pio/libdeps/ or .piolibdeps/ depending on version
avionmesh_dir = None
for libdeps_name in [".pio/libdeps", ".piolibdeps"]:
    search = project_dir
    for _ in range(10):
        check_dir = os.path.join(search, libdeps_name)
        if os.path.isdir(check_dir):
            for item in os.listdir(check_dir):
                candidate = os.path.join(check_dir, item, "avionmesh")
                if os.path.isdir(candidate) and os.path.isdir(os.path.join(candidate, "include")):
                    avionmesh_dir = candidate
                    break
        if avionmesh_dir:
            break
        search = os.path.dirname(search)
    if avionmesh_dir:
        break

if not avionmesh_dir:
    avionmesh_dir = os.path.join(project_dir, ".pio", "libdeps", "esphome-avionmesh", "avionmesh")

avionmesh_include = os.path.join(avionmesh_dir, "include")
recsrmesh_include = os.path.join(avionmesh_dir, "external", "recsrmesh", "include")

cmake_path = os.path.join(project_dir, "src", "CMakeLists.txt")
patched_content = f"""\
FILE(GLOB_RECURSE app_sources ${{CMAKE_SOURCE_DIR}}/src/*.*)

idf_component_register(
    SRCS ${{app_sources}}
    INCLUDE_DIRS "{avionmesh_include}" "{recsrmesh_include}"
    REQUIRES mbedtls bt
)
"""

with open(cmake_path, "w") as f:
    f.write(patched_content)
