#ifdef __faasm
extern "C"
{
#include "faasm/host_interface.h"
}

#include <faasm/faasm.h>
#else
#include "libs/s3/S3Wrapper.hpp"
#endif

#include "tless.h"

#include <iostream>
#include <map>
#include <sstream>
#include <string>

std::map<std::string, int> wordCount = {
    {"JavaScript", 0},
    {"Java", 0},
    {"PHP", 0},
    {"Python", 0},
    {"C#", 0},
    {"C++", 0},
    {"Ruby", 0},
    {"CSS", 0},
    {"Objective-C", 0},
    {"Perl", 0},
    {"Scala", 0},
    {"Haskell", 0},
    {"MATLAB", 0},
    {"Clojure", 0},
    {"Groovy", 0}
};

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

std::string serialiseWordCount()
{
    std::string result;

    for (const auto& [key, val] : wordCount) {
        result += key + ":" + std::to_string(val) + ",";
    }
    result.pop_back();

    return result;
}

/* Mapper Function - Step 2 of MapReduce Workflow
 *
 * The mapper function taks as an input an S3 path, and, as an output, writes
 * a serialized JSON to S3 with the partial counts of different programming
 * languages.
 */
int main(int argc, char** argv)
{
    // TODO: this is currently hardcoded
    std::string bucketName = "tless";
    int id;
    std::string s3ObjectKey;

#ifdef __faasm
    // Get the object key as an input
    int inputSize = faasmGetInputSize();
    char inputChar[inputSize];
    faasmGetInput((uint8_t*)inputChar, inputSize);

    std::string tmpStr(inputChar, inputChar + inputSize);
    auto parts = splitByDelimiter(tmpStr, ":");
    if (parts.size() != 2) {
        std::cerr << "word-count(mapper): error parsing driver input" << std::endl;
        return 1;
    }

    id = std::stoi(parts.at(0));
    s3ObjectKey = parts.at(1);
#else
    if (argc != 3) {
        std::cerr << "word-count(mapper): error parsing driver input" << std::endl;
        return 1;
    }

    id = std::stoi(argv[1]);
    s3ObjectKey = argv[2];

    s3::initS3Wrapper();
    s3::S3Wrapper s3cli;
#endif

    std::string us = "mapper-" + std::to_string(id);

    if (!tless::checkChain("word-count", "mapper", id)) {
        std::cerr << "word-count(" << us << "): error checking TLess chain" << std::endl;
        return 1;
    }

    // Read object from S3
    uint8_t* keyBytes;
#ifdef __faasm
    int keyBytesLen;

    int ret =
      __faasm_s3_get_key_bytes(bucketName.c_str(), s3ObjectKey.c_str(), &keyBytes, &keyBytesLen);
    if (ret != 0) {
        std::cerr << "word-count(" << us << "): error getting key bytes from key: "
                  << s3ObjectKey
                  << " (bucket: "
                  << bucketName
                  << " )"
                  << std::endl;
        return 1;
    }
#else
    auto keyBytesVec = s3cli.getKeyBytes(bucketName, s3ObjectKey);
    keyBytes = keyBytesVec.data();
#endif

    // Read object file line-by-line, and map the inputs to our word-count map
    std::stringstream stringStream((char*) keyBytes);
    std::string currentLine;
    while (std::getline(stringStream, currentLine, '\n')) {
        for (auto& [key, val] : wordCount) {
            if (currentLine.find(key) != std::string::npos) {
                val += 1;
            }
        }
    }

    // Work-out the serialised payload and directory
    auto thisWordCount = serialiseWordCount();
    std::string resultsKey = "word-count/outputs/" + us;
    std::cout << "word-count(" << us << "): writting result to " << resultsKey << std::endl;
#ifdef __faasm
    // Overwrite the results key
    ret =
      __faasm_s3_add_key_bytes(bucketName.c_str(),
                               resultsKey.c_str(),
                               (void*) thisWordCount.c_str(),
                               thisWordCount.size(),
                               true);
#else
    s3cli.addKeyStr(bucketName, resultsKey, thisWordCount);
    s3::shutdownS3Wrapper();
#endif

    return 0;
}
