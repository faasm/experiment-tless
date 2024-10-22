#include "tless.h"
// Both tless::aes256gcm and tles::sha256 are declared in this header
#include "tless_aes.h"
#ifdef __faasm
// For the time being, this library is only needed in Faasm
#include "tless_jwt.h"
#endif
#include "tless_abe.h"
#include "dag.h"
#include "utils.h"

// Faasm includes
#ifdef __faasm
#include <faasm/core.h>
#else
#include "../s3/S3Wrapper.hpp"
#endif

#include <iostream>
#ifdef TLESS_UBENCH
#include <chrono>
#endif
#include <string>
#include <vector>
#include <utility>

#define AES256CM_NONCE_SIZE 12

#ifdef TLESS_UBENCH
typedef std::chrono::time_point<std::chrono::high_resolution_clock> TimePoint;
std::vector<std::pair<std::string, TimePoint>> timePoints;

#define NOW std::chrono::high_resolution_clock::now()

void prettyPrintTimePoints()
{
    TimePoint prevTimePoint;
    std::string prevLabel;

    std::cout << "###################### TLess Timing #####################" << std::endl;
    for (const auto& [label, tp] : timePoints) {
        if (!prevLabel.empty()) {
            std::chrono::duration<double, std::milli> duration = tp - prevTimePoint;
            std::cout << prevLabel
                      << " to "
                      << label
                      << ": "
                      << duration.count()
                      << " ms"
                      << std::endl;
        }

        prevLabel = label;
        prevTimePoint = tp;
    }

    std::chrono::duration<double, std::milli> total =
      timePoints.at(timePoints.size() -1).second - timePoints.at(0).second;
    std::cout << "Total: " << total.count() << " ms" << std::endl;
    std::cout << "#########################################################" << std::endl;
}

#endif

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
    const char* envVar = std::getenv("TLESS_MODE");
    if (envVar && (std::string(envVar) == "on")) {
        return true;
    }

    return false;
#endif
}

// Specific per-TEE mechanism to get attestation
static bool validHardwareAttestation()
{
#ifdef __faasm
    // Get SGX quote and send it to MAA, get JWT in return
#ifdef TLESS_UBENCH
    timePoints.push_back(std::make_pair("get-hw-att-begin", NOW));
#endif
    char* jwt;
    int32_t jwtSize;
    __tless_get_attestation_jwt(&jwt, &jwtSize);
    std::string jwtStr(jwt);

#ifdef TLESS_UBENCH
    timePoints.push_back(std::make_pair("get-hw-att-end", NOW));
#endif

    // Verify JWT signature
    bool valid = tless::jwt::verify(jwtStr);
    if (!valid) {
        return false;
    }

    // Check signed JWT comes from the expected attestation service and has
    // the same MRENCLAVE we do
    if (!tless::jwt::checkProperty(jwt, "jku", ATT_PROVIDER_JKU)) {
        std::cout << "tless: error: failed to validate JWT JKU" << std::endl;
        return false;
    }

    // To compare the MRENCLAVE with the one in the JWT, we need to convert
    // the raw bytes from the measurement to a hex string
    std::vector<uint8_t> mrEnclave(MRENCLAVE_SIZE);
    __tless_get_mrenclave(mrEnclave.data(), mrEnclave.size());
    std::string mrEnclaveHex = tless::utils::byteArrayToHexString(mrEnclave.data(), mrEnclave.size());
    if (!tless::jwt::checkProperty(jwt, "sgx-mrenclave", mrEnclaveHex)) {
        std::cout << "tless: error: failed to validate MrEnclave" << std::endl;
        return false;
    }

    // FIXME(tless-prod): as a consequence of a valid attestation, the MAA
    // would have provided us with the TEE shared identity, wrapped in the
    // public key we provide as part of the enclave held data of the report.
    // We still do not have implemented this functionality in the MAA
#else
    // For SNP we could just use snpguest crate from here
#endif

#ifdef TLESS_UBENCH
    timePoints.push_back(std::make_pair("hw-att-validate-end", NOW));
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
bool checkChain(const std::string& workflow, const std::string& function, int id)
{
    if (!on()) {
        return true;
    }

#ifdef TLESS_UBENCH
    timePoints.push_back(std::make_pair("begin", NOW));
#endif

#ifndef __faasm
    s3::initS3Wrapper();
    s3::S3Wrapper s3cli;
#endif

    // -----------------------------------------------------------------------
    // 0. Fetch DAG
    // 0.1. Get DAG string from S3
    // 0.2. Calculate DAG hex digest
    // -----------------------------------------------------------------------

    std::vector<uint8_t> serializedDag;
    std::string dagKey = workflow + "/dag";
#ifdef __faasm
    serializedDag = tless::utils::doGetKeyBytes("tless", dagKey);
#else
    serializedDag = s3cli.getKeyBytes("tless", dagKey);
#endif
    auto dag = tless::dag::deserialize(serializedDag);

    // Also calculate the hex-digest of the serialized DAG
    std::vector<uint8_t> hashedDag = tless::sha256::hash(serializedDag);
    std::string dagHexDigest = tless::utils::byteArrayToHexString(hashedDag.data(), hashedDag.size());

#ifdef TLESS_UBENCH
    timePoints.push_back(std::make_pair("end-fetch-exec", NOW));
#endif

    // -----------------------------------------------------------------------
    // 1. Get TEE certificate
    // 1.1. Generate quote w/ public key for MAA
    // 1.2. Send quote to MAA for validation
    // 1.3. Receive quote and valiate signature
    // -----------------------------------------------------------------------

    if (!validHardwareAttestation()) {
        return false;
    }

    // FIXME(tless-prod): this value would be returned by the MAA upon a
    // succesful attestation, encrypted with the public key we provided as
    // part of the original attestation bundle
    std::string teeIdentity = "G4NU1N3TL3SST33";

    // -----------------------------------------------------------------------
    // 2. Bootstrap CP-ABE context
    // 2.1. Fetch encrypted context
    // 2.2. Decrypt it with the TEE identity that we obtained from the previous
    //      step
    // -----------------------------------------------------------------------

#ifdef TLESS_UBENCH
    timePoints.push_back(std::make_pair("begin-fetch-dec-cpabe", NOW));
#endif

    // Fetch the (encrypted) CP-ABE context from S3
    std::vector<uint8_t> ctCtx;
    // This key is hard-coded in tlessctl/src/tasks/dag.rs
    std::string cpAbeCtxKey = workflow + "/crypto/cp-abe-ctx";
#ifdef __faasm
    ctCtx = tless::utils::doGetKeyBytes("tless", cpAbeCtxKey);
#else
    ctCtx = s3cli.getKeyBytes("tless", cpAbeCtxKey);
#endif

    // Decrypt the CP-ABE context
    std::vector<uint8_t> nonceCtx(ctCtx.begin(), ctCtx.begin() + AES256CM_NONCE_SIZE);
    std::vector<uint8_t> ctCtxTrimmed(ctCtx.begin() + AES256CM_NONCE_SIZE, ctCtx.end());
    auto ptCtx = tless::aes256gcm::decrypt(DEMO_SYM_KEY, nonceCtx, ctCtxTrimmed);

#ifdef TLESS_UBENCH
    timePoints.push_back(std::make_pair("end-fetch-dec-cpabe", NOW));
#endif

#ifdef TLESS_UBENCH
    timePoints.push_back(std::make_pair("begin-fetch-dec-cert-chain", NOW));
#endif

    // Fetch the certificate chain for us. The certificate chain is wrapped
    // around an AES-encrypted bundle, and then CP-ABE encrypted
    std::vector<uint8_t> ctAesCertChain;
    // TODO: for the time being, we all share one cert-chain to measure the
    // overheads of TLess, albeit not fully functional
    // std::string certChainKey = workflow + "/cert-chain/" + function;
    std::string certChainKey = workflow + "/cert-chains/test";
#ifdef __faasm
    ctAesCertChain = tless::utils::doGetKeyBytes("tless", certChainKey);
#else
    ctAesCertChain = s3cli.getKeyBytes("tless", certChainKey);
#endif

    // Decrypt the AES bundle around certificate chain
    std::vector<uint8_t> nonceCertChain(ctAesCertChain.begin(), ctAesCertChain.begin() + AES256CM_NONCE_SIZE);
    std::vector<uint8_t> ctCertChain(ctAesCertChain.begin() + AES256CM_NONCE_SIZE, ctAesCertChain.end());
    auto ptAesCertChain = tless::aes256gcm::decrypt(DEMO_SYM_KEY, nonceCertChain, ctCertChain);

#ifdef TLESS_UBENCH
    timePoints.push_back(std::make_pair("end-fetch-dec-cert-chain", NOW));
#endif

#ifdef TLESS_UBENCH
    timePoints.push_back(std::make_pair("begin-gen-ecf-id", NOW));
#endif

    // Initialize CP-ABE context
    auto& ctx = tless::abe::CpAbeContextWrapper::get(tless::abe::ContextFetchMode::FromBytes, ptCtx);

    // Generate our set of attributes from the place we occupy in the dag
    std::vector<std::string> attributes = {teeIdentity, dagHexDigest};
    // TODO: this does not work well for functions with more than one parent!
    auto expectedChain = tless::dag::getCallChain(dag, function);

    for (int i = 0; i < expectedChain.size() - 1; i++) {
        attributes.push_back(expectedChain.at(i));
    }

    // Now use our attributes to decrypt the actual contents of the cert chain
    auto certChain = ctx.cpAbeDecrypt(attributes, ptAesCertChain);
    if (certChain.empty()) {
        std::cerr << "tless: error decrypting certificate chain" << std::endl;
        return false;
    }

#ifdef TLESS_UBENCH
    timePoints.push_back(std::make_pair("end-gen-ecf-id", NOW));
#endif

    // Note that, succesful validation, implies that the function is called
    // in the right order. That being said, we double-check it here too
    std::vector<std::string> actualChain = tless::dag::getFuncChainFromCertChain(certChain);
    /* TODO: un-comment this checks when we properly propagate messages
    if (actualChain.size() != expectedChain.size()) {
        std::cerr << "tless: error: size mismatch between expected ("
                  << expectedChain.size()
                  << ") and actual ("
                  << actualChain.size()
                  << ") chains"
                  << std::endl;
        return false;
    }
    for (int i = 0; i < actualChain.size(); i++) {
        if (i == 0) {
            if (actualChain.at(0) != TLESS_CHAIN_GENESIS) {
                std::cerr << "tless: error: certificate chain has wrong beginning" << std::endl;
                return false;
            }
        } else {
            if (actualChain.at(i) != expectedChain.at(i - 1)) {
                std::cerr << "tless: error in cert chain (got: "
                          << actualChain.at(i)
                          << " - expected: "
                          << expectedChain.at(i - 1)
                          << ")" << std::endl;
                return false;
            }
        }
    }
    if (expectedChain.at(expectedChain.size() - 1) != function) {
        std::cerr << "tless: error in cert chain (got: "
                  << expectedChain.at(expectedChain.size() - 1)
                  << " - expected: "
                  << function
                  << ")" << std::endl;
        return false;
    }
    */

    std::cout << "tless: certificate chain validated!" << std::endl;

    // -----------------------------------------------------------------------
    // 3. Get execution token
    // 3.1. Fetch execution token from storage
    // 3.2. Decrypt function code
    // -----------------------------------------------------------------------

#ifdef TLESS_UBENCH
    timePoints.push_back(std::make_pair("begin-fetch-exec-token", NOW));
#endif

    // Attempt to get the exec-token (tolerate missing)
    std::vector<uint8_t> execToken;
    std::string execTokenKey = workflow + "/exec-tokens/" + function + "-" + std::to_string(id);
#ifdef __faasm
    execToken = tless::utils::doGetKeyBytes("tless", execTokenKey, true);
#else
    execToken = s3cli.getKeyBytes("tless", execTokenKey, true);
#endif

    // Check there are no bytes
    if (!execToken.empty()) {
        std::cerr << "tless: error: exec token already taken" << std::endl;
        return false;
    }

    // TODO: consider encrypting/signing?
    std::string tokenBytes = "exec-token";
#ifdef __faasm
    tless::utils::doAddKeyBytes("tless", execTokenKey, tokenBytes);
#else
    s3cli.addKeyStr("tless", execTokenKey, tokenBytes);
    // NOTE: deliberately not shutdown s3 here, as it may be used after
#endif


#ifdef TLESS_UBENCH
    timePoints.push_back(std::make_pair("end-fetch-exec-token", NOW));
#endif

#ifdef TLESS_UBENCH
    timePoints.push_back(std::make_pair("end", NOW));
    prettyPrintTimePoints();
#endif

    return true;
}

int32_t chain(const std::string& workflow,
              const std::string& parentFuncName,
              int parentIdx,
              const std::string& funcName,
              int idx,
              const std::string& inputData)
{
    if (!on()) {
#ifdef __faasm
        return faasmChainNamed(funcName.c_str(), (uint8_t*) inputData.c_str(), inputData.size());
#else
        return 0;
#endif
    }

    std::cout << "tless(chain): extending certificate chain and chaining" << std::endl;

    // TODO: actually propagate the certificate chain
    std::string key = workflow + "/cert-chains/" + funcName + "-" + std::to_string(parentIdx) + "-" + std::to_string(idx);
    std::string keyBytes = "UPDATE_ME";
#ifdef __faasm
    tless::utils::doAddKeyBytes("tless", key, keyBytes);
#else
    s3::initS3Wrapper();
    s3::S3Wrapper s3cli;
    s3cli.addKeyStr("tless", key, keyBytes);
#endif


#ifdef __faasm
    return faasmChainNamed(funcName.c_str(), (uint8_t*) inputData.c_str(), inputData.size());
#else
    return 0;
#endif
}

#ifdef __faasm
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
