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
std::vector<uint8_t> doGetKeyBytes(const std::string& bucketName, const std::string& key, bool tolerateMissing)
{
    uint8_t* ptr;
    int32_t len;
    int ret =
      __faasm_s3_get_key_bytes("tless", key.c_str(), &ptr, &len, tolerateMissing);

    if (len == 0 && tolerateMissing) {
        return std::vector<uint8_t>();
    }

    std::vector<uint8_t> keyBytes(len);
    std::memcpy(keyBytes.data(), ptr, len);
    std::free(ptr);

    return keyBytes;
}

void doAddKeyBytes(const std::string& bucketName, const std::string& key, const std::string& bytes)
{
      __faasm_s3_add_key_bytes("tless",
                               key.c_str(),
                               (void*) bytes.c_str(),
                               bytes.size(),
                               true);
}

void doAddKeyBytes(const std::string& bucketName, const std::string& key, const std::vector<uint8_t>& bytes)
{
      __faasm_s3_add_key_bytes("tless",
                               key.c_str(),
                               (void*) bytes.data(),
                               bytes.size(),
                               true);
}
#endif
}
