#ifdef __faasm
extern "C"
{
#include "faasm/host_interface.h"
}

#include <faasm/faasm.h>
#else
#include <cstdlib>
#include <fstream>
#include <iostream>
#include "libs/s3/S3Wrapper.hpp"
#endif

#include "trade.h"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

std::vector<std::string> splitByDelimiter(std::string stringCopy, const std::string& delimiter)
{
    std::vector<std::string> splitString;

    size_t pos = 0;
    std::string token;
    while ((pos = stringCopy.find(delimiter)) != std::string::npos) {
        splitString.push_back(stringCopy.substr(0, pos));
        stringCopy.erase(0, pos + delimiter.length());
    }
    splitString.push_back(stringCopy);

    return splitString;
}

std::string join(const std::vector<std::string>& stringList, const std::string& delimiter)
{
    if (stringList.size() == 0) {
        return "";
    }

    std::string result = stringList.at(0);
    for (int i = 1; i < stringList.size(); i++) {
        result += delimiter;
        result += stringList.at(i);
    }

    return result;
}

/* Fetch Public Data - FINRA workflow
 */
int main(int argc, char** argv)
{
    // TODO: the bucket name is currently hardcoded
    std::string bucketName = "tless";
    std::string s3DataFile;

#ifdef __faasm
    // Get the object key as an input
    int inputSize = faasmGetInputSize();
    char inputChar[inputSize];
    faasmGetInput((uint8_t*)inputChar, inputSize);

    s3DataFile.assign(inputChar);
#else
    if (argc != 2) {
        std::cerr << "finra(fetch-public): error: cannot parse input from driver"
                  << std::endl;
        return 1;
    }
    s3DataFile = argv[1];

    s3::initS3Wrapper();
    s3::S3Wrapper s3cli;
#endif

    std::cout << "finra(fetch-public): fetching public trades data from "
              << s3DataFile
              << std::endl;

    std::string csvData;
#ifdef __faasm
    uint8_t* keyBytes;
    int keyBytesLen;

    int ret =
      __faasm_s3_get_key_bytes(bucketName.c_str(), s3DataFile.c_str(), &keyBytes, &keyBytesLen);
    if (ret != 0) {
        std::cerr << "finra(fetch-public): error: error getting bytes from key: "
                  << s3DataFile << "(bucket: " << bucketName << ")"
                  << std::endl;
        return 1;
    }
    // WARNING: can we avoid the copy
    csvData.assign((char*) keyBytes, (char*) keyBytes + keyBytesLen);
#else
    csvData = s3cli.getKeyStr(bucketName, s3DataFile);
#endif

    // Structure CSV data, and upload to S3 for actual audit processing
    std::cout << "finra(fetch-public): structuring and serializing trade data"
              << std::endl;

    std::vector<TradeData> tradeData = tless::finra::loadCSVFromString(csvData);
    std::vector<uint8_t> serializedTradeData = tless::finra::serializeTradeVector(tradeData);

    // Upload structured data to S3
    std::string key = "finra/outputs/fetch-public/trades";
    std::cout << "finra(fetch-public): uploading structured trade data to "
              << key
              << std::endl;
#ifdef __faasm
    // Overwrite the results
    ret =
      __faasm_s3_add_key_bytes(bucketName.c_str(),
                               key.c_str(),
                               serializedTradeData.data(),
                               serializedTradeData.size(),
                               true);
    if (ret != 0) {
        std::cerr << "finra(fetch-public): error uploading trade data"
                  << std::endl;
        return 1;
    }
#else
    s3cli.addKeyStr(bucketName, key, serializedTradeData);
    s3::shutdownS3Wrapper();
#endif

    return 0;
}
