cmake_minimum_required(VERSION 3.8.0)
project(tless-workflows)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (CMAKE_SYSTEM_NAME STREQUAL "WASI")
    # For Faasm functions, we add `_faasm_zygote` as an exported symbol
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} -Xlinker --export=_faasm_zygote"
    )
    message(STATUS "TLess: detected wasm build (sysroot=$ENV{FAASM_WASM_SYSROOT})")
    set(CMAKE_EXECUTABLE_SUFFIX ".wasm")

    # Build TLess C++ library
    # add_subdirectory(libs/tless)

    set(TLESS_WORKFLOW_LIBS faasm)
    # set(TLESS_WORKFLOW_LIBS faasm tless)

else ()
    add_subdirectory(libs/s3)

    message(STATUS "TLess: detected native build")

    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
    set(TLESS_WORKFLOW_LIBS
        TLess::s3
    )
endif ()

function(faasm_wflow_func wflow_name func_name file_name)
    set(target_name "${wflow_name}_${func_name}")
    add_executable(${target_name} ${file_name})
    target_link_libraries(${target_name} ${TLESS_WORKFLOW_LIBS})

    if (NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
        target_include_directories(${target_name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
    endif ()
endfunction(faasm_wflow_func)

add_subdirectory(finra)
add_subdirectory(ml-inference)
add_subdirectory(ml-training)
add_subdirectory(word-count)
