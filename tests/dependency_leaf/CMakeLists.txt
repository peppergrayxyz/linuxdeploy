add_library(dependency_leaf SHARED dependency_leaf.cpp dependency_leaf.h)
set_target_properties(dependency_leaf PROPERTIES OUTPUT_NAME dependency_leaf)
target_include_directories(dependency_leaf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(dependency_leaf_static STATIC dependency_leaf.cpp dependency_leaf.h)
target_include_directories(dependency_leaf_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ABI-incompatible replacement used by real relocation-failure tests.
add_library(dependency_leaf_missing_symbol SHARED missing_symbol.cpp)
