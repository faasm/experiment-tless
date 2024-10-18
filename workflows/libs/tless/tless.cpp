#include "tless.h"
#include "tless_aes.h"
#ifdef __faasm
// For the time being, this library is only needed in Faasm
#include "tless_jwt.h"
#endif
#include "tless_abe.h"
#include "utils.h"

// Faasm includes
#ifdef __faasm
extern "C"
{
#include "faasm/host_interface.h"
}
#include <faasm/core.h>
#else
#include "../s3/S3Wrapper.hpp"
#endif

#include <iostream>
#include <string>
#include <vector>
#include <utility>

// FIXME(tless-prod): this symmetric key is shared by all TEEs, and would be
// provided by the MAA upon succesful attestation
std::vector<uint8_t> DEMO_SYM_KEY = {
    0xf0, 0x0d, 0x48, 0x2e, 0xca, 0x21, 0xfb, 0x13,
    0xec, 0xf0, 0x01, 0x48, 0xba, 0x60, 0x01, 0x76,
    0x6e, 0x56, 0xbb, 0xa5, 0xff, 0x9b, 0x11, 0x9d,
    0xd6, 0xfa, 0x96, 0x39, 0x2b, 0x7c, 0x1a, 0x0d
};

namespace tless {
bool on()
{
#ifdef __faasm
    // Returns 0 if TLess features are enabled
    return __tless_is_enabled() == 0;
#else
    return true;
#endif
}

// Specific per-TEE mechanism to get attestation
static bool validHardwareAttestation()
{
#ifdef __faasm
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

    // FIXME(tless-prod): as a consequence of a valid attestation, the MAA
    // would have provided us with the TEE shared identity, wrapped in the
    // public key we provide as part of the enclave held data of the report.
    // We still do not have implemented this functionality in the MAA
#endif

    return true;
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

    // -----------------------------------------------------------------------
    // 1. Get TEE certificate
    // 1.1. Generate quote w/ public key for MAA
    // 1.2. Send quote to MAA for validation
    // 1.3. Receive quote and valiate signature
    // -----------------------------------------------------------------------

    if (!validHardwareAttestation()) {
        return false;
    }

    // -----------------------------------------------------------------------
    // 2. Bootstrap CP-ABE context
    // 2.1. Fetch encrypted context
    // 2.2. Decrypt it with the TEE identity that we obtained from the previous
    //      step
    // -----------------------------------------------------------------------

    // Fetch the (encrypted) CP-ABE context from S3
    std::vector<uint8_t> encCtx;
#ifdef __faasm
    uint8_t* encCtxPtr;
    int32_t encCtxLen;
    int ret =
      __faasm_s3_get_key_bytes("tless", "word-count/hello", &encCtxPtr, &encCtxLen);
    encCtx.assign(encCtxPtr, encCtxPtr + encCtxLen);
#else
    s3::initS3Wrapper();
    s3::S3Wrapper s3cli;
    encCtx = s3cli.getKeyBytes("tless", "word-count/hello");
#endif

    std::cout << "test: enc context len is: " << encCtx.size() << std::endl;
    std::cout << "test: enc context ptr is: " << encCtx.data() << std::endl;
      // __faasm_s3_get_key_bytes("tless", "word-count/crypto/cp-abe-ctx", &encCtx, &encCtxLen);
    std::vector<uint8_t> nonce(encCtx.begin(), encCtx.begin() + 12);
    std::vector<uint8_t> cipherText(encCtx.begin() + 12, encCtx.end());
    std::cout << "about to decrypt " << std::endl;
    auto plainText = tless::aes256gcm::decrypt(DEMO_SYM_KEY, nonce, cipherText);
    std::string test((char*) plainText.data(), plainText.size());
    // FIXME: this test is still not working
    std::cout << "test decryption: " << test << std::endl;

    // TODO: MAA could encrypt a special nonce to generate the (shared) TEE
    // identity from (for the time-being, use a hard-coded string)
    // FIXME: this is insecure, because it could be read by inspecting the
    // enclave binary
    std::string teeIdentity = "G4NU1N3_TL3SS_T33";

    // -----------------------------------------------------------------------
    // 3. Get execution token
    // 3.1. Fetch execution token from storage
    // 3.2. Decrypt function code
    // -----------------------------------------------------------------------

    std::cout << "still VERY strong!" << std::endl;

    return true;
}

#ifdef __faasm
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
#endif
}
