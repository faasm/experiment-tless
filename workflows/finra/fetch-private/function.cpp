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

// Private stock portfolio
Portfolio portfolio = {
    {
        {"AAPL", 100, 150.0},
        {"GOOG", 50, 2800.0}
    }
};

/* Fetch Private Data - FINRA workflow
 *
 * This workflow simulates loading data from a private stock holding. In this
 * case we hard-code the data in the function code.
 */
int main(int argc, char** argv)
{
    // TODO: the bucket name is currently hardcoded
    std::string bucketName = "tless";

#ifndef __faasm
    s3::initS3Wrapper();
    s3::S3Wrapper s3cli;
#endif

    std::cout << "finra(fetch-private): fetching & uploading private portfolio data"
              << std::endl;

    // Fetch is a no-op

    // Serialize the portfolio
    std::vector<uint8_t> serializedPortfolio = tless::finra::serializePortfolio(portfolio);

    // Upload structured data to S3
    std::string key = "finra/outputs/fetch-private/portfolio";
    std::cout << "finra(fetch-private): uploading structured portfolio data to "
              << key
              << std::endl;
#ifdef __faasm
    // Overwrite the results
    int ret =
      __faasm_s3_add_key_bytes(bucketName.c_str(),
                               key.c_str(),
                               serializedPortfolio.data(),
                               serializedPortfolio.size(),
                               true);
    if (ret != 0) {
        std::cerr << "finra(fetch-private): error uploading portfolio data"
                  << std::endl;
        return 1;
    }
#else
    s3cli.addKeyStr(bucketName, key, serializedPortfolio);
    s3::shutdownS3Wrapper();
#endif

    return 0;
}
