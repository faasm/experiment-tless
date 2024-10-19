# -----------------------------------------
# C++ implementation of the TLess library
# -----------------------------------------

add_library(tless
    tless.cpp
    utils.cpp
)

if (CMAKE_SYSTEM_NAME STREQUAL "WASI")
    set(TLESS_LIBRARIES
        faasm
        rabe-cpp
        rabe
        tless-jwt-cpp
        tless-jwt
    )
else ()
    set(TLESS_LIBRARIES
        TLess::s3
        # Order matters: librabe-cpp must preceede librabe
        "/usr/local/lib/rabe/librabe-cpp.a"
        "/usr/local/lib/rabe/librabe.a"
    )
    target_include_directories(tless PUBLIC "/usr/include/rabe")
endif()

target_link_libraries(tless PUBLIC ${TLESS_LIBRARIES})

if (CMAKE_SYSTEM_NAME STREQUAL "WASI")
    # Manually install the .imports file so that we can link against it
    add_custom_command(
        TARGET tless
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_LIST_DIR}/libtless.imports ${CMAKE_CURRENT_BINARY_DIR}/libtless.imports
        COMMENT "Created ${PROJECT_BINARY_DIR}/libtless.imports"
    )
endif ()

add_library(TLess::core ALIAS tless)
