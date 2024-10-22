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

#include "tless.h"
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

/* Run Audit Rule - FINRA workflow
 */
int main(int argc, char** argv)
{
    // TODO: the bucket name is currently hardcoded
    std::string bucketName = "tless";

    int id;
    std::string tradesKey;
    std::string portfolioKey;

#ifdef __faasm
    // Get the object key as an input
    int inputSize = faasmGetInputSize();
    char inputChar[inputSize];
    faasmGetInput((uint8_t*)inputChar, inputSize);

    std::string tmpStr(inputChar, inputChar + inputSize);
    auto parts = splitByDelimiter(tmpStr, ":");
    if (parts.size() != 3) {
        std::cerr << "finra(audit): error parsing driver input" << std::endl;
        return 1;
    }

    id = std::stoi(parts.at(0));
    tradesKey = parts.at(1);
    portfolioKey = parts.at(2);
#else
    if (argc != 4) {
        std::cerr << "finra(audit): error parsing driver input" << std::endl;
        return 1;
    }

    id = std::stoi(argv[1]);
    tradesKey = argv[2];
    portfolioKey = argv[3];

    s3::initS3Wrapper();
    s3::S3Wrapper s3cli;
#endif
    std::string us = "audit-" + std::to_string(id);

    if (!tless::checkChain("finra", "audit", id)) {
        std::cerr << "finra(" << us << "): error checking TLess chain" << std::endl;
        return 1;
    }

    std::cout << "finra(" << us << "): fetching public trades data from "
              << tradesKey
              << std::endl;

    std::vector<uint8_t> tradeData;
#ifdef __faasm
    uint8_t* keyBytes;
    int keyBytesLen;

    int ret =
      __faasm_s3_get_key_bytes(bucketName.c_str(), tradesKey.c_str(), &keyBytes, &keyBytesLen);
    if (ret != 0) {
        std::cerr << "finra(" << us << "): error: error getting bytes from key: "
                  << tradesKey << "(bucket: " << bucketName << ")"
                  << std::endl;
        return 1;
    }
    // WARNING: can we avoid the copy
    tradeData.assign((char*) keyBytes, (char*) keyBytes + keyBytesLen);
#else
    tradeData = s3cli.getKeyBytes(bucketName, tradesKey);
#endif

    std::cout << "finra(" << us << "): fetching portfolio data from "
              << portfolioKey
              << std::endl;

    std::vector<uint8_t> portfolioData;
#ifdef __faasm
    keyBytes = nullptr;
    keyBytesLen = 0;

    ret =
      __faasm_s3_get_key_bytes(bucketName.c_str(), portfolioKey.c_str(), &keyBytes, &keyBytesLen);
    if (ret != 0) {
        std::cerr << "finra(" << us << "): error: error getting bytes from key: "
                  << portfolioKey << "(bucket: " << bucketName << ")"
                  << std::endl;
        return 1;
    }
    // WARNING: can we avoid the copy
    portfolioData.assign((char*) keyBytes, (char*) keyBytes + keyBytesLen);
#else
    portfolioData = s3cli.getKeyBytes(bucketName, portfolioKey);
#endif

    std::cout << "finra(" << us << "): de-serializing data" << std::endl;
    std::vector<TradeData> trades = tless::finra::deserializeTradeVector(tradeData);
    Portfolio portfolio = tless::finra::deserializePortfolio(portfolioData);

    std::cout << "finra(" << us << "): running audit rule on " << trades.size() << " trades ..." << std::endl;
    std::string auditResults;
    for (const auto& trade : trades) {
        bool insideTradeDetected =
          tless::finra::rules::potentialInsiderTrade(portfolio, trades, trade.date);
        auditResults += std::to_string(insideTradeDetected) + ",";
    }
    auditResults.pop_back();
    std::cout << "finra(" << us << "): done running audit rule!" << std::endl;

    // Upload structured data to S3
    std::string key = "finra/outputs/audit/" + us;
    std::cout << "finra(" << us << "): uploading audit results to "
              << key
              << std::endl;
#ifdef __faasm
    // Overwrite the results
    ret =
      __faasm_s3_add_key_bytes(bucketName.c_str(),
                               key.c_str(),
                               (uint8_t*) auditResults.c_str(),
                               auditResults.size(),
                               true);
    if (ret != 0) {
        std::cerr << "finra(" << us << "): error uploading trade data"
                  << std::endl;
        return 1;
    }
#else
    s3cli.addKeyStr(bucketName, key, auditResults);
    s3::shutdownS3Wrapper();
#endif

    return 0;
}
