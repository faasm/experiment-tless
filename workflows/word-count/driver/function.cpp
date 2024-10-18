#ifdef __faasm
#include <faasm/core.h>
#include "tless.h"
#endif

#include <iostream>
#include <stdio.h>
#include <string>
#include <tuple>
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

/* Driver Function - MapReduce workflow
 *
 * This function acts as a "coordinator" for the MapReduce workflow. It
 * reduces the amount of workflow-specific logic that we need to implement in
 * Faasm.
 *
 * In a TLess context, the coordinator can be interpreted as "all the things
 * that could go wrong" during execution of a confidential serverless workflow.
 *
 * As an input, this workflow gets the S3 path read data from.
 */
int main(int argc, char** argv)
{
    if (argc != 2) {
        printf("word-count(driver): error: workflow must be invoked with one parameter: <s3_prefix>\n");
        return 1;
    }
    std::string s3prefix = argv[1];

    // 1. Invoke one instance of the splitter function with an S3 path as an
    // input. The splitter function will, in turn, traverse the S3 directory
    // and invoke one mapper function per file in the directory.
    //
    // The return value of the splitter function is a list of message ids for
    // the mapper function
    printf("word-count(driver): invoking one splitter function\n");
#ifdef __faasm
    // Call splitter
    int splitterId = tless::chain("splitter", s3prefix);
#endif

#ifdef __faasm
    auto [result, splitterOutput] = tless::wait(splitterId);
    if (result != 0) {
        printf("word-count(driver): error: splitter execution failed with rc %i\n", result);
        return 1;
    }
#else
    std::string splitterOutput;
#endif

    // Get all message ids from output
    std::vector<std::string>  mapperIds = splitByDelimiter(splitterOutput, ",");

    // 2. Wait for all mapper functions to have finished
    printf("word-count(driver): waiting for %zu mapper functions...\n", mapperIds.size());
    for (auto mapperIdStr : mapperIds) {
        int mapperId = std::stoi(mapperIdStr);
#ifdef __faasm
        std::tie(result, std::ignore) = tless::wait(mapperId, true);
        if (result != 0) {
            printf("word-count(driver): error: mapper execution (id: %i) failed with rc %i\n", mapperId, result);
            return 1;
        }
#endif
    }

    // 3. Invoke one reducer function to aggreagate all results
    std::string s3result = "word-count/outputs/mapper-";
    printf("word-count(driver): invoking one reducer function on prefix %s\n",
           s3result.c_str());
#ifdef __faasm
    // Call reducer and await
    int reducerId = tless::chain("reducer", s3result);
    std::tie(result, std::ignore) = tless::wait(reducerId, true);
    if (result != 0) {
        printf("word-count(driver): error: reducer failed with rc %i\n", result);
        return 1;
    }
#endif

    std::string output = "word-count(driver): workflow executed succesfully!";
    std::cout << output << std::endl;
#ifdef __faasm
    faasmSetOutput(output.c_str(), output.size());
#endif

    return 0;
}
