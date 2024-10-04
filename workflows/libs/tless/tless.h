#pragma once

// Include Faasm's core for basic chaining functionalities
#include <faasm/core.h>
#include <string>
#include <utility>

// We define with C-linkage all the external symbols that a TLess ECF needs
// from the runtime environment. These are implemented by the runtime outside
// of WASM.
extern "C" {
int32_t __tless_get_dag_size();
}

/* Main TLess C++ API
 *
 * MISSING:
 * - Decrypt/Encrypt function input/output
 * - Decrypt/Encrypt S3 input/output
 */
namespace tless {
// Return whether we must use TLess chaining protection mechanisms or not
bool on();

// Validate that the attested call chain is consistent with the DAG and with
// the function we are executing (i.e. us). This method is the main entrypoint
// implementing the chaining validation protocol from the paper
bool checkChain();

// Chain a function by name, and return the function id to wait-on
int32_t chain(const std::string& funcName, const std::string& inputData);

// Wait for a function by its id, and get its output and return code
std::pair<int, std::string> wait(int32_t functionId, bool ignoreOutput=false);
}
