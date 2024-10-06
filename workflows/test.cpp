#include "tless.h"

#include <cstdint>
#include <iostream>
#include <string>

int main()
{
    /*
    char* jwt;
    int32_t jwtSize;

    // JWT is heap-allocated
    __tless_get_attestation_jwt(&jwt, &jwtSize);

    std::string jwtStr(jwt);

    bool valid = tless::jwt::verify(jwtStr);
    if (valid) {
        std::cout << "JWT is valid!" << std::endl;
    } else {
        std::cout << "JWT is invalid :-(" << std::endl;
    }

    // Check JWT is for us, and not for someone else
    std::vector<uint8_t> mrEnclave(MRENCLAVE_SIZE);
    __tless_get_mrenclave(mrEnclave.data(), mrEnclave.size());

    // std::string jkuStr(ATT_PROVIDER_JKU);
    if (!tless::jwt::checkProperty(jwt, "jku", ATT_PROVIDER_JKU)) {
        std::cout << "Failed to validate JWT JKU" << std::endl;
        return 0;
    } else {
        std::cout << "Validated JWT JKU!" << std::endl;
    }
    // To compare the MRENCLAVE with the one in the JWT, we need to convert
    // the raw bytes from the measurement to a hex string
    std::string mrEnclaveHex = tless::utils::byteArrayToHexString(mrEnclave.data(), mrEnclave.size());
    if (!tless::jwt::checkProperty(jwt, "sgx-mrenclave", mrEnclaveHex)) {
        std::cout << "Failed to validate MrEnclave" << std::endl;
        return 0;
    }
    */

    if (tless::checkChain()) {
        std::cout << "Chain is valid!" << std::endl;
    } else {
        std::cout << "Chain is invalid :-(" << std::endl;
    }


    return 0;
}
