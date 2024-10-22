cmake_minimum_required(VERSION 3.8.0)
project(tless-workflows)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build auxiliary libraries
if (CMAKE_SYSTEM_NAME STREQUAL "WASI")
    message(STATUS "TLess: detected wasm build (sysroot=$ENV{FAASM_WASM_SYSROOT})")
    set(CMAKE_EXECUTABLE_SUFFIX ".wasm")
else ()
    message(STATUS "TLess: detected native build")
    set(CMAKE_CXX_FLAGS "-g ${CMAKE_CXX_FLAGS} -O3")

    add_subdirectory(libs/s3)
endif ()

# Build TLess C++ library
add_subdirectory(libs/tless)

# Un-comment to enable U-Bench
# target_compile_definitions(tless PUBLIC TLESS_UBENCH)

# Prepare variables for workflow compilation
if (CMAKE_SYSTEM_NAME STREQUAL "WASI")
    set(TLESS_WORKFLOW_HEADERS ${CMAKE_CURRENT_LIST_DIR}/libs/tless)
    set(TLESS_WORKFLOW_LIBS faasm tless)

    # Test executable for debugging purposes
    # TODO: rename to ubench
    add_executable(tless_test test.cpp)
    target_compile_definitions(tless_test PUBLIC TLESS_UBENCH)
    target_link_libraries(tless_test ${TLESS_WORKFLOW_LIBS})
    target_include_directories(tless_test PUBLIC ${TLESS_WORKFLOW_HEADERS})
else ()
    set(TLESS_WORKFLOW_HEADERS
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/libs/tless/
        /usr/include/rabe/
    )
    set(TLESS_WORKFLOW_LIBS
        TLess::core
        TLess::s3
        # Order matters: librabe-cpp must preceede librabe
        "/usr/local/lib/rabe/librabe-cpp.a"
        "/usr/local/lib/rabe/librabe.a"
    )

    # Test executable for debugging purposes
    # TODO: re-purpose as ubench
    add_executable(tless_test_native test.cpp)
    target_link_libraries(tless_test_native ${TLESS_WORKFLOW_LIBS})
    target_include_directories(tless_test_native PUBLIC ${TLESS_WORKFLOW_HEADERS})
endif ()

function(faasm_wflow_func wflow_name func_name file_name)
    set(target_name "${wflow_name}_${func_name}")
    add_executable(${target_name} ${file_name})
    target_link_libraries(${target_name} ${TLESS_WORKFLOW_LIBS})
    target_include_directories(${target_name} PUBLIC ${TLESS_WORKFLOW_HEADERS})
endfunction(faasm_wflow_func)

add_subdirectory(finra)
add_subdirectory(ml-inference)
add_subdirectory(ml-training)
add_subdirectory(word-count)
