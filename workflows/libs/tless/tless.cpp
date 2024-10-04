#include "tless.h"

#include <faasm/core.h>
#include <string>
#include <utility>

namespace tless {
bool on()
{
    // Get env. variable
    return false;
}

bool checkChain()
{
    if (!on()) {
        return true;
    }

    // 0. Get execution request (i.e. faabric::Message?)

    // 1. Get TEE certificate
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
