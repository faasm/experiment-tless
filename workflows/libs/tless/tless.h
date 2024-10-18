#pragma once

#include <string>
#include <utility>

#ifdef __faasm
// Include Faasm's core for basic chaining functionalities
#include <faasm/core.h>
// Defines that we get from the SGX SDK (move elsewhere?)
// mr_enclave in the SGX report has type sgx_measurement_t which is a SHA256
// digest (see sgx_report.h)
#define MRENCLAVE_SIZE 32
#define ATT_PROVIDER_JKU "https://faasmattprov.eus2.attest.azure.net/certs"

// We define with C-linkage all the external symbols that a TLess ECF needs
// from the runtime environment. These are implemented by the runtime outside
// of WASM. For correct WASM linkage, these symbols also need to be listed in
// workflows/libs/tless/libtless.imports
extern "C" {
int32_t __tless_is_enabled();
void __tless_get_attestation_jwt(char** jwt, int32_t* jwtSize);
void __tless_get_mrenclave(uint8_t* buf, int32_t bufSize);
}
#endif

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
// implementing the chaining validation protocol from the paper. For a
// detailed explanation of the protocol, see the comment in the source file
bool checkChain(const std::string& workflow, const std::string& function, int id);

// Chain a function by name, and return the function id to wait-on
int32_t chain(const std::string& funcName, const std::string& inputData);

#ifdef __faasm
// Wait for a function by its id, and get its output and return code
std::pair<int, std::string> wait(int32_t functionId, bool ignoreOutput=false);
#endif
}
