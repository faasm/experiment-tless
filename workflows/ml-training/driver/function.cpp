#ifdef __faasm
#include <faasm/core.h>
#endif

#include <iostream>
#include <string>
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

/* Driver Function - ML training workflow
 *
 * This function acts as a "coordinator" for the ML training workflow. It
 * reduces the amount of workflow-specific logic that we need to implement in
 * Faasm.
 *
 * As an input, this workflow gets the S3 path read data from, and two numbers:
 * - numPca: the number of PCA analysis to start in parallel.
 * - numRf: the number of random forest trees to store in parallel
 */
int main(int argc, char** argv)
{
#ifdef __faasm
    if (argc != 4) {
        printf("ml-training(driver): usage: <s3_path_mnist> <num_pca> <num_rf>\n");
        return 1;
    }
    std::string s3prefix = argv[1];

    // 1. Invoke one instance of the partition function with an S3 path as an
    // input and the number of PCA component analysis to spawn.
    //
    // The return value of the partition function is ...
    printf("ml-training(driver): invoking one partition function\n");
    // Call splitter
    std::string splitterInput = s3prefix + ":" + argv[2] + ":" + argv[3];
    int partitionId = faasmChainNamed("partition", (uint8_t*) splitterInput.c_str(), splitterInput.size());

    char* partitionOutput;
    int partitionOutputLen;
    int result = faasmAwaitCallOutput(partitionId, &partitionOutput, &partitionOutputLen);
    if (result != 0) {
        printf("ml-training(driver): error: partition execution failed with rc %i\n", result);
        return 1;
    }

    // Get all message ids from output
    std::vector<std::string>  pcaIds = splitByDelimiter(partitionOutput, ",");

    // Wait for all PCA functions to have finished
    printf("ml-training(driver): waiting for %zu PCA functions... (out: %s)\n", pcaIds.size(), partitionOutput);
    std::vector<int> trainIds;
    for (auto pcaIdStr : pcaIds) {
        int pcaId = std::stoi(pcaIdStr);
        char* trainOutput;
        int trainOutputLen;
        // TODO: will have to get the output, and wait for all training functions
        result = faasmAwaitCallOutput(pcaId, &trainOutput, &trainOutputLen);
        if (result != 0) {
            printf("ml-training(driver): error: PCA execution (id: %i) failed with rc %i\n", pcaId, result);
            return 1;
        }

        auto thisTrainIds = splitByDelimiter(trainOutput, ",");
        for (const auto tid : thisTrainIds) {
            trainIds.push_back(std::stoi(tid));
        }
    }

    // Wait for all train functions to have finished
    printf("ml-training(driver): waiting for %zu RF train functions...\n", trainIds.size());
    int i = 0;
    for (auto trainId : trainIds) {
        result = faasmAwaitCall(trainId);
        if (result != 0) {
            printf("ml-training(driver): error: RF train execution (id: %i) failed with rc %i\n", trainId, result);
            return 1;
        }
    }

    // Finally, invoke one validation function
    printf("ml-training(driver): invoking one validation function\n");
    std::string validationInput = "ml-training/outputs/rf-";
    int validationId = faasmChainNamed("validation", (uint8_t*) validationInput.c_str(), validationInput.size());
    result = faasmAwaitCall(validationId);
    if (result != 0) {
        printf("ml-training(driver): error: validation execution (id: %i) failed with rc %i\n",
               validationId,
               result);
        return 1;
    }

    std::string output = "ml-training(driver): workflow executed succesfully!";
    std::cout << output << std::endl;
    faasmSetOutput(output.c_str(), output.size());
#endif

    return 0;
}
