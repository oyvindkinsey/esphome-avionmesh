"""Pre-build script: add mbedtls REQUIRES and library sources to CMakeLists.txt."""
Import("env")

import os

project_dir = env.subst("$PROJECT_DIR")

# The ESPHome project dir is under .esphome/build/<name>/
# Walk up to find the real project root (where avion-gateway.yaml lives)
# by looking for the lib/ directory
search = project_dir
for _ in range(10):
    if os.path.isdir(os.path.join(search, "lib", "recsrmesh")):
        break
    search = os.path.dirname(search)

lib_dir = os.path.join(search, "lib")
recsrmesh_src = os.path.realpath(os.path.join(lib_dir, "recsrmesh", "src"))
avionmesh_src = os.path.realpath(os.path.join(lib_dir, "avionmesh", "src"))

cmake_path = os.path.join(project_dir, "src", "CMakeLists.txt")
patched_content = f"""\
FILE(GLOB_RECURSE app_sources ${{CMAKE_SOURCE_DIR}}/src/*.*)
FILE(GLOB_RECURSE recsrmesh_sources {recsrmesh_src}/*.cpp)
FILE(GLOB_RECURSE avionmesh_sources {avionmesh_src}/*.cpp)

idf_component_register(
    SRCS ${{app_sources}} ${{recsrmesh_sources}} ${{avionmesh_sources}}
    REQUIRES mbedtls bt
)
"""

with open(cmake_path, "w") as f:
    f.write(patched_content)
