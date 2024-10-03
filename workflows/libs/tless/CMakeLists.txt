# -----------------------------------------
# C++ implementation of the TLess library
# -----------------------------------------

add_library(tless tless.cpp)

target_link_libraries(tless PUBLIC
    faasm
    rabe-cpp
    rabe
)
