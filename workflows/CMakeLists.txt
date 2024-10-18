cmake_minimum_required(VERSION 3.8.0)
project(tless-workflows)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (CMAKE_SYSTEM_NAME STREQUAL "WASI")
    message(STATUS "TLess: detected wasm build (sysroot=$ENV{FAASM_WASM_SYSROOT})")
    set(CMAKE_EXECUTABLE_SUFFIX ".wasm")

    # Build TLess C++ library
    add_subdirectory(libs/tless)

    set(TLESS_WORKFLOW_HEADERS ${CMAKE_CURRENT_LIST_DIR}/libs/tless)
    set(TLESS_WORKFLOW_LIBS faasm tless)

    # Test executable for debugging purposes
    # TODO: delete me
    add_executable(tless_test test.cpp)
    target_link_libraries(tless_test ${TLESS_WORKFLOW_LIBS})
    target_include_directories(tless_test PUBLIC ${TLESS_WORKFLOW_HEADERS})
else ()
    add_subdirectory(libs/s3)

    message(STATUS "TLess: detected native build")

    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")

    set(TLESS_WORKFLOW_HEADERS ${CMAKE_CURRENT_LIST_DIR})
    set(TLESS_WORKFLOW_LIBS
        TLess::s3
    )
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
