# -----------------------------------------
# C++ implementation of the TLess library
# -----------------------------------------

add_library(tless
    tless.cpp
    utils.cpp
)

target_link_libraries(tless PUBLIC
    faasm
    rabe-cpp
    rabe
    tless-jwt-cpp
    tless-jwt
)

# Manually install the .imports file so that we can link against it
add_custom_command(
    TARGET tless
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_LIST_DIR}/libtless.imports ${CMAKE_CURRENT_BINARY_DIR}/libtless.imports
    COMMENT "Created ${PROJECT_BINARY_DIR}/libtless.imports"
)
