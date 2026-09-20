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
