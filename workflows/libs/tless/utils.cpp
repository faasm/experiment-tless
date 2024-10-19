#include "utils.h"

#ifdef __faasm
extern "C"
{
#include "faasm/host_interface.h"
}
#endif
#include <iomanip>
#include <sstream>

namespace tless::utils {
std::string byteArrayToHexString(const uint8_t* data, int dataSize)
{
    std::stringstream ss;
    ss << std::hex;

    for (int i = 0; i < dataSize; ++i) {
        ss << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    }

    return ss.str();
}

#ifdef __faasm
std::vector<uint8_t> doGetKeyBytes(const std::string& bucketName, const std::string& key)
{
    uint8_t* ptr;
    int32_t len;
    int ret =
      __faasm_s3_get_key_bytes("tless", key.c_str(), &ptr, &len);

    std::vector<uint8_t> keyBytes(len);
    std::memcpy(keyBytes.data(), ptr, len);
    std::free(ptr);

    return keyBytes;
}
#endif
}
