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

/* Load Model Function - ML Inference Workflow
 */
int main(int argc, char** argv)
{
    if (!tless::checkChain("ml-inference", "load", 0)) {
        std::cerr << "ml-inference(load): error checking TLess chain" << std::endl;
        return 1;
    }

    // TODO: the bucket name is currently hardcoded
    std::string bucketName = "tless";
    std::string s3prefix;

#ifdef __faasm
    // Get the object key as an input
    int inputSize = faasmGetInputSize();
    char inputChar[inputSize];
    faasmGetInput((uint8_t*)inputChar, inputSize);

    s3prefix.assign(inputChar, inputChar + inputSize);
#else
    if (argc != 2) {
        std::cerr << "ml-inference(load): error parsing driver input" << std::endl;
        return 1;
    }

    s3prefix = argv[1];

    s3::initS3Wrapper();
    s3::S3Wrapper s3cli;
#endif

    // Get the list of files for each PCA function
    std::cout << "ml-inference(load): loading model data from "
              << s3prefix
              << std::endl;
    std::vector<std::string> s3files;

#ifdef __faasm
    int numKeys = __faasm_s3_get_num_keys_with_prefix(
      bucketName.c_str(), s3prefix.c_str());

    char** keysBuffer = (char**) malloc(numKeys * sizeof(char*));
    int* keysBufferLens = (int*) malloc(numKeys * sizeof(int32_t));

    __faasm_s3_list_keys_with_prefix(
      bucketName.c_str(), s3prefix.c_str(), keysBuffer, keysBufferLens);

    for (int i = 0; i < numKeys; i++) {
        std::string tmpString;
        tmpString.assign(keysBuffer[i], keysBuffer[i] + keysBufferLens[i]);
        s3files.push_back(tmpString);
    }

#else
    s3files = s3cli.listKeys(bucketName, s3prefix);
#endif

    // NOTE: for the time being, loading only re-uploads
    for (const auto& file : s3files) {
        // First download the file
        std::string fileContents;
#ifdef __faasm
        uint8_t* keyBytes;
        int keyBytesLen;

        int ret =
          __faasm_s3_get_key_bytes(bucketName.c_str(), file.c_str(), &keyBytes, &keyBytesLen);
        if (ret != 0) {
            std::cerr << "ml-inference(load): error: error getting bytes from key: "
                      << file << "(bucket: " << bucketName << ")"
                      << std::endl;
            return 1;
        }
        fileContents.assign((char*) keyBytes, (char*) keyBytes + keyBytesLen);
#else
        fileContents = s3cli.getKeyStr(bucketName, file);
#endif

        // Now upload as input for ML inference workflow
        auto fileParts = splitByDelimiter(file, "/");
        auto fileName = fileParts.at(fileParts.size() - 1);
        std::string key = "ml-inference/outputs/load/" + fileName;
#ifdef __faasm
        // Overwrite the results
        ret =
          __faasm_s3_add_key_bytes(bucketName.c_str(),
                                   key.c_str(),
                                   fileContents.data(),
                                   fileContents.size(),
                                   true);
        if (ret != 0) {
            std::cerr << "ml-inference(load): error uploading model data for ML inference" << std::endl;
            return 1;
        }
#else
        s3cli.addKeyStr(bucketName, key, fileContents);
#endif
    }

#ifndef __faasm
    // Add a file to let know we are done loading
    s3cli.addKeyStr(bucketName, "ml-inference/outputs/load/done.txt", "done");
    s3::shutdownS3Wrapper();
#endif

    return 0;
}
