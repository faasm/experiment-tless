#pragma once

// Include Faasm's core for basic chaining functionalities
#include <faasm/core.h>

// We define with C-linkage all the external symbols that a TLess ECF needs
// from the runtime environment. These are implemented by the runtime outside
// of WASM.
extern "C" {
int32_t __tless_get_dag_size();
}

namespace tless {
// Main TLess API. We shadow the Faasm API..?

// Return whether we must use TLess chaining protection mechanisms or not
bool on();

// Chain a function by name, and return the function id to wait-on
int32_t chain(const std::string& funcName, const std::string& inputData);

// Wait for a function by its id, and get its output
std::string wait(int32_t functionId);
}
