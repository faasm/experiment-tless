#include "tless.h"
#include "tless_aes.h"
#include "tless_jwt.h"
#include "tless_abe.h"
#include "utils.h"

// For Faasm S3
extern "C"
{
#include "faasm/host_interface.h"
}

#include <iostream>
#include <faasm/core.h>
#include <string>
#include <vector>
#include <utility>

// FIXME: this symmetric key is shared by all TEEs, and would be provided by
// the MAA upon succesful attestation
std::vector<uint8_t> DEMO_SYM_KEY = {
    0xf0, 0x0d, 0x48, 0x2e, 0xca, 0x21, 0xfb, 0x13,
    0xec, 0xf0, 0x01, 0x48, 0xba, 0x60, 0x01, 0x76,
    0x6e, 0x56, 0xbb, 0xa5, 0xff, 0x9b, 0x11, 0x9d,
    0xd6, 0xfa, 0x96, 0x39, 0x2b, 0x7c, 0x1a, 0x0d
};

namespace tless {
bool on()
{
    // Returns 0 if TLess features are enabled
    return __tless_is_enabled() == 0;
}

/* TLess chain validation protocol
 * 0. Get execution request
 * 1. Get TEE certificate:
 *  1.1. Get SGX quote
 *  1.2. Send it to Microsoft's Attestation Service, and get a JWT in return
 *  1.3. Validate that the JWT comes from Microsofr, and get
 */
bool checkChain()
{
    // TODO: we must add this method at the entrypoint of _each_ function
    if (!on()) {
        return true;
    }

    // 0. Get execution request (i.e. faabric::Message?)
    // TODO somehow get these two values
    std::string dag = "foo-bar";
    std::string certChain = "foo-bar-baz";

    // 1. Get TEE certificate
    // 1.1. Generate quote w/ public key for MAA
    // 1.2. Send quote to MAA for validation
    // 1.3. Decrypt it here

    // Get SGX quote and send it to MAA, get JWT in return
    char* jwt;
    int32_t jwtSize;
    __tless_get_attestation_jwt(&jwt, &jwtSize);
    std::string jwtStr(jwt);

    // Verify JWT signature
    bool valid = tless::jwt::verify(jwtStr);
    if (!valid) {
        return false;
    }

    // Check signed JWT comes from the expected attestation service and has
    // the same MRENCLAVE we do
    if (!tless::jwt::checkProperty(jwt, "jku", ATT_PROVIDER_JKU)) {
        std::cout << "Failed to validate JWT JKU" << std::endl;
        return false;
    }

    // To compare the MRENCLAVE with the one in the JWT, we need to convert
    // the raw bytes from the measurement to a hex string
    std::vector<uint8_t> mrEnclave(MRENCLAVE_SIZE);
    __tless_get_mrenclave(mrEnclave.data(), mrEnclave.size());
    std::string mrEnclaveHex = tless::utils::byteArrayToHexString(mrEnclave.data(), mrEnclave.size());
    if (!tless::jwt::checkProperty(jwt, "sgx-mrenclave", mrEnclaveHex)) {
        std::cout << "Failed to validate MrEnclave" << std::endl;
        return false;
    }

    // Fetch the (encrypted) CP-ABE context from S3
    uint8_t* encCtx;
    int32_t encCtxLen;
    int ret =
      __faasm_s3_get_key_bytes("tless", "word-count/hello", &encCtx, &encCtxLen);
      // __faasm_s3_get_key_bytes("tless", "word-count/crypto/cp-abe-ctx", &encCtx, &encCtxLen);
    std::vector<uint8_t> nonce(encCtx, encCtx + 12);
    std::vector<uint8_t> cipherText(encCtx + 12, encCtx + encCtxLen - 12);
    auto plainText = tless::aes256gcm::decrypt(DEMO_SYM_KEY, nonce, cipherText);
    std::string test((char*) plainText.data(), plainText.size());
    // FIXME: this test is still not working
    std::cout << "test decryption: " << test << std::endl;

    // TODO: MAA could encrypt a special nonce to generate the (shared) TEE
    // identity from (for the time-being, use a hard-coded string)
    // FIXME: this is insecure, because it could be read by inspecting the
    // enclave binary
    std::string teeIdentity = "G4NU1N3_TL3SS_T33";

    // Fetch the CP-ABE context
    /*
    auto& ctx = tless::abe::CpAbeContextWrapper::get(tless::abe::ContextFetchMode::FromS3);

    // Prepare decryption of the certificate chain
    std::vector<std::string> attributes = {teeIdentity, dag};
    auto actualPlainText = ctx.cpAbeDecrypt(attributes, certChain);

    if (actualPlainText.empty()) {
        std::cout << "Decryption of the certificate chain failed!" << std::endl;
        return false;
    }
    */

    std::cout << "still strong!" << std::endl;

    return true;
}

int32_t chain(const std::string& funcName, const std::string& inputData)
{
    if (!on()) {
        return faasmChainNamed(funcName.c_str(), (uint8_t*) inputData.c_str(), inputData.size());
    }

    return -1;
}

std::pair<int, std::string> wait(int32_t functionId, bool ignoreOutput)
{
    if (!on()) {
        if (!ignoreOutput) {
            // TODO: think about memory ownership here
            char* output;
            int outputLen;
            int result = faasmAwaitCallOutput(functionId, &output, &outputLen);

            return std::make_pair(result, output);
        }

        int result = faasmAwaitCall(functionId);
        return std::make_pair(result, "");
    }

    return std::make_pair(-1, "");
}
}
