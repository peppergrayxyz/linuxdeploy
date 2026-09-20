add_executable(simple_executable simple_executable.cpp)
target_link_libraries(simple_executable simple_library dependency_parent pthread)

add_executable(simple_executable_static simple_executable.cpp)
target_link_libraries(simple_executable_static simple_library_static dependency_parent_static pthread)
target_link_options(simple_executable_static PUBLIC
    -static
    $<$<CXX_COMPILER_ID:GNU>:-no-pie>
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc>
    $<$<CXX_COMPILER_ID:GNU>:-static-libstdc++>
)

# static-pie executables contain a PT_DYNAMIC segment but must still be treated as statically linked
# the toolchain needs a static libc built for static-pie, so this fixture is optional
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-static-pie")
check_c_source_compiles("int main(void) { return 0; }" HAVE_STATIC_PIE)
unset(CMAKE_REQUIRED_FLAGS)

if(HAVE_STATIC_PIE)
    add_executable(simple_executable_static_pie static_pie_executable.c)
    target_compile_options(simple_executable_static_pie PRIVATE -fPIE)
    target_link_options(simple_executable_static_pie PUBLIC -static-pie)
endif()
