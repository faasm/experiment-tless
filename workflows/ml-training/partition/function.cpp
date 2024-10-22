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
    if (!tless::checkChain("ml-training", "partition", 0)) {
        std::cerr << "ml-training(partition): error checking TLess chain" << std::endl;
        return 1;
    }

    // TODO: the bucket name is currently hardcoded
    std::string bucketName = "tless";
    std::string s3dir;
    int numPcaFuncs;
    int numTrainFuncs;

#ifdef __faasm
    // Get the object key as an input
    int inputSize = faasmGetInputSize();
    char inputChar[inputSize];
    faasmGetInput((uint8_t*)inputChar, inputSize);

    std::string tmpStr(inputChar, inputChar + inputSize);
    auto parts = splitByDelimiter(tmpStr, ":");
    if (parts.size() != 3) {
        std::cerr << "ml-training(partition): error parsing driver input" << std::endl;
        return 1;
    }

    s3dir = parts.at(0);
    numPcaFuncs = std::stoi(parts.at(1));
    numTrainFuncs = std::stoi(parts.at(2));
#else
    if (argc != 4) {
        std::cerr << "ml-training(partition): error parsing driver input" << std::endl;
        return 1;
    }

    s3dir = argv[1];
    numPcaFuncs = std::stoi(argv[2]);
    numTrainFuncs = std::stoi(argv[3]);

    s3::initS3Wrapper();
    s3::S3Wrapper s3cli;
#endif

    // Get the list of files for each PCA function
    std::cout << "ml-training(partition): partitioning "
              << s3dir
              << " between "
              << numPcaFuncs
              << " PCA component functions"
              << std::endl
              << "ml-training(partition): (into "
              << numTrainFuncs
              << " train functions)"
              << std::endl;

    std::vector<std::vector<std::string>> s3files(numPcaFuncs);

#ifdef __faasm
    // In Faasm we need to do a bit of work because: (i) we can not pass
    // structured objects (i.e. vectors) through the WASM calling interface,
    // and (ii) we have not implmented prefix listing, so we need to filter
    // out entries manually
    int numKeys = __faasm_s3_get_num_keys_with_prefix(
      bucketName.c_str(), s3dir.c_str());

    // In this case, we need to be careful because we have many keys, so we
    // must heap allocate both structures
    char** keysBuffer = (char**) malloc(numKeys * sizeof(char*));
    int* keysBufferLens = (int*) malloc(numKeys * sizeof(int32_t));

    __faasm_s3_list_keys_with_prefix(
      bucketName.c_str(), s3dir.c_str(), keysBuffer, keysBufferLens);

    // Pre-allocate the size of each string
    std::vector<int> sizePerPca(numPcaFuncs);
    std::vector<int> numPerPca(numPcaFuncs);
    for (int i = 0; i < numKeys; i++) {
        // We add 1 to account for the "," separating the file names
        sizePerPca.at(i % numPcaFuncs) += keysBufferLens[i] + 1;
        numPerPca.at(i % numPcaFuncs) += 1;
    }
    // Disccount one extra comma
    for (int i = 0; i < numPcaFuncs; i++) {
        sizePerPca.at(i) -= 1;
    }

    // Serialize the input char** into N different char* to upload them back
    // to S3
    std::vector<char*> s3filesPtr(numPcaFuncs);
    for (int i = 0; i < numPcaFuncs; i++) {
        s3filesPtr.at(i) = (char*) malloc(sizePerPca.at(i));
    }

    std::vector<int> offsets(numPcaFuncs);
    std::vector<int> counts(numPcaFuncs);
    for (int i = 0; i < numKeys; i++) {
        int pcaIdx = i % numPcaFuncs;
        int offset = offsets.at(pcaIdx);

        std::memcpy(s3filesPtr.at(pcaIdx) + offsets.at(pcaIdx), keysBuffer[i], keysBufferLens[i]);
        counts.at(pcaIdx) += 1;

        if (counts.at(pcaIdx) < numPerPca.at(pcaIdx)) {
            *(s3filesPtr.at(pcaIdx) + offset + keysBufferLens[i]) = ',';
            offsets.at(pcaIdx) += keysBufferLens[i] + 1;
        } else {
            offsets.at(pcaIdx) += keysBufferLens[i];
        }
    }
#else
    auto rawS3files = s3cli.listKeys(bucketName, s3dir);

    std::cout << "ml-training(partition): partitioning "
              << rawS3files.size()
              << " files between "
              << numPcaFuncs
              << " PCA functions"
              << std::endl;

    for (int i = 0; i < rawS3files.size(); i++) {
        int pcaIdx = i % numPcaFuncs;
        s3files.at(pcaIdx).push_back(rawS3files.at(i));
    }
#endif

    // Upload one file per calling function
    for (int i = 0; i < numPcaFuncs; i++) {
        std::string key = "ml-training/outputs/partition/pca-" + std::to_string(i);
#ifdef __faasm
        std::string_view fileNames(s3filesPtr.at(i));
        // Overwrite the results
        int ret =
          __faasm_s3_add_key_bytes(bucketName.c_str(),
                                   key.c_str(),
                                   (void*) s3filesPtr.at(i),
                                   sizePerPca.at(i),
                                   true);
        if (ret != 0) {
            std::cerr << "ml-training(partition): error uploading filenames for PCA functions" << std::endl;
            return 1;
        }
#else
        std::string fileNames = join(s3files.at(i), ",");
        s3cli.addKeyStr(bucketName, key, fileNames);
#endif
    }

    // Chain to all PCA functions? maybe just return. Or make each PCA
    // function call two RF
    // Call two PCA, tell each PCA how many training functions to spawn
    int numTrainPerPca = numTrainFuncs / numPcaFuncs;
    std::cout << "ml-training(partition): invoking "
              << numPcaFuncs
              << " partition functions with "
              << numTrainPerPca
              << " training functions each"
              << std::endl;

    std::vector<std::string> pcaFuncsIds;
    for (int i = 0; i < numPcaFuncs; i++) {
        std::string key = "ml-training/outputs/partition/pca-" + std::to_string(i);
        std::string pcaInput = std::to_string(i) + ":" + key + ":" + std::to_string(numTrainPerPca);
#ifdef __faasm
        // int pcaId = faasmChainNamed("pca", (uint8_t*) pcaInput.c_str(), pcaInput.size());
        int pcaId = tless::chain("ml-training", "partition", 0, "pca", i, pcaInput);
        pcaFuncsIds.push_back(std::to_string(pcaId));
#endif
    }

    // Tell the driver the ids of the PCA funcs to wait on them
#ifdef __faasm
    std::string outputStr = join(pcaFuncsIds, ",");
    faasmSetOutput(outputStr.c_str(), outputStr.size());
#else
    s3::shutdownS3Wrapper();
#endif

    return 0;
}
