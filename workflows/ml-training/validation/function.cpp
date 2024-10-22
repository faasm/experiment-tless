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

/* Partition Function - ML Training Workflow
 *
 * This function takes as an input an S3 path and a number of PCA functions,
 * and splits the total number of images between the number of PCA functions.
 * It stores in ml-training/partition-output/ as many keys as functions it
 * will invoke, each key containing the list of files the functions need to
 * load and perform PCA on.
 */
int main(int argc, char** argv)
{
    // TODO: the bucket name is currently hardcoded
    std::string bucketName = "tless";
    std::string s3prefix;

    if (!tless::checkChain("ml-training", "validation", 0)) {
        std::cerr << "ml-training(validation): error checking TLess chain" << std::endl;
        return 1;
    }

#ifdef __faasm
    // Get the object key as an input
    int inputSize = faasmGetInputSize();
    char inputChar[inputSize];
    faasmGetInput((uint8_t*)inputChar, inputSize);

    s3prefix.assign(inputChar);
#else
    if (argc != 2) {
        std::cerr << "ml-training(validation): error parsing driver input" << std::endl;
        return 1;
    }
    s3prefix = argv[1];

    s3::initS3Wrapper();
    s3::S3Wrapper s3cli;
#endif

    // Get the list of files for each PCA function
    std::cout << "ml-training(validation): validating rf model data from "
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

    // NOTE: for the time being, validate only re-uploads
    for (const auto& file : s3files) {
        // First download the file
        std::string fileContents;
#ifdef __faasm
        uint8_t* keyBytes;
        int keyBytesLen;

        int ret =
          __faasm_s3_get_key_bytes(bucketName.c_str(), file.c_str(), &keyBytes, &keyBytesLen);
        if (ret != 0) {
            printf("ml-training(validation): error: error getting bytes from key: %s (bucket: %s)\n",
                   file.c_str(),
                   bucketName.c_str());
        }
        fileContents.assign((char*) keyBytes, (char*) keyBytes + keyBytesLen);
#else
        fileContents = s3cli.getKeyStr(bucketName, file);
#endif

        // Now upload as input for ML inference workflow
        auto fileParts = splitByDelimiter(file, "/");
        auto fileName = fileParts.at(fileParts.size() - 1);
        std::string key = "ml-inference/model/" + fileName;
#ifdef __faasm
        // Overwrite the results
        ret =
          __faasm_s3_add_key_bytes(bucketName.c_str(),
                                   key.c_str(),
                                   fileContents.data(),
                                   fileContents.size(),
                                   true);
        if (ret != 0) {
            std::cerr << "ml-training(validation): error uploading model data for ML inference" << std::endl;
            return 1;
        }
#else
        s3cli.addKeyStr(bucketName, key, fileContents);
#endif
    }

    // Add result key that we can wait-on in Knative
    std::string resultsStr = "done!";
    std::string resultKey = "ml-training/outputs/done.txt";
    std::cout << "ml-training(validation): writting done file to "
              << resultKey
              << std::endl;
#ifdef __faasm
    // Overwrite the results key
    int ret =
      __faasm_s3_add_key_bytes(bucketName.c_str(),
                               resultKey.c_str(),
                               (void*) resultsStr.c_str(),
                               resultsStr.size(),
                               true);
#else
    s3cli.addKeyStr(bucketName, resultKey, resultsStr);
    s3::shutdownS3Wrapper();
#endif

    return 0;
}
