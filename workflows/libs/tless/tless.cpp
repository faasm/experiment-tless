#include <faasm/faasm.core.h>
#include <tless.h>

namespace tless {
bool tless::on()
{
    // Get env. variable
    return false;
}

int32_t tless::chain(const std::string& funcName, const std::string& inputData)
{
    if (!on()) {
        return faasmChainNamed(funcName, (uint8_t*) inputData.c_str(), inputData.size());
    }
}
}
