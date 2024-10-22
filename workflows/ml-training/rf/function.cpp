#ifdef __faasm
// WARNING: this needs to preceed the OpenCV includes
// Even when threading is completely disabled, OpenCV assumes the C++ library
// has been built with threading support, and typedefs (no-ops) these two
// symbols. To prevent an undefined symbol error, we define them here.
namespace std {
    class recursive_mutex {
    public:
        void lock() {}
        bool try_lock() { return true; }
        void unlock() {}
    };

    template <typename T>
    class lock_guard {
    public:
        explicit lock_guard(T&) {}
    };
}
#endif

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

#include <filesystem>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/ml.hpp>
#include <sstream>
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

cv::Mat deserializeMat(const std::vector<uint8_t>& buffer) {
    // Extract matrix metadata: rows, cols, type
    const int* header = reinterpret_cast<const int*>(buffer.data());
    int rows = header[0];
    int cols = header[1];
    int type = header[2];

    // Extract matrix data
    const uint8_t* dataPtr = buffer.data() + sizeof(int) * 3;
    return cv::Mat(rows, cols, type, const_cast<uint8_t*>(dataPtr)).clone();
}

std::vector<uint8_t> serializeForest(const cv::Ptr<cv::ml::RTrees>& forest) {
    std::vector<uint8_t> buffer;
    cv::FileStorage fs(".yml", cv::FileStorage::WRITE | cv::FileStorage::MEMORY);
    forest->write(fs);

    // Get serialized data
    std::string modelData = fs.releaseAndGetString();
    buffer.assign(modelData.begin(), modelData.end());

    return buffer;
}

int main(int argc, char** argv) {
    std::string bucketName = "tless";
    std::string dataKey;
    std::string labelsKey;
    int id;
    int pid;

#ifdef __faasm
    // Get the object key as an input
    int inputSize = faasmGetInputSize();
    char inputChar[inputSize];
    faasmGetInput((uint8_t*)inputChar, inputSize);

    std::string tmpStr(inputChar, inputChar + inputSize);
    auto parts = splitByDelimiter(tmpStr, ":");
    if (parts.size() != 4) {
        std::cerr << "ml-training(rf): error parsing pca input" << std::endl;
        return 1;
    }

    pid = std::stoi(parts.at(0));
    id = std::stoi(parts.at(1));
    dataKey = parts.at(2);
    labelsKey = parts.at(3);
#else
    if (argc != 5) {
        std::cerr << "ml-training(pca): error parsing driver input" << std::endl;
        return 1;
    }

    pid = std::stoi(argv[1]);
    id = std::stoi(argv[2]);
    dataKey = argv[3];
    labelsKey = argv[4];

    s3::initS3Wrapper();
    s3::S3Wrapper s3cli;
#endif

    std::string us = "rf-" + std::to_string(pid) + "-" + std::to_string(id);

    if (!tless::checkChain("ml-training", "rf", pid * 10 + id)) {
        std::cerr << "ml-training(" << us << "): error checking TLess chain" << std::endl;
        return 1;
    }

    std::cout << "ml-training(" << us << "): training random forest on data "
              << dataKey
              << std::endl
              << "ml-training(" << us << "): using labels "
              << labelsKey
              << std::endl;

    std::vector<uint8_t> imageData;
#ifdef __faasm
    uint8_t* keyBytes;
    int keyBytesLen;
    int ret =
      __faasm_s3_get_key_bytes(bucketName.c_str(), dataKey.c_str(), &keyBytes, &keyBytesLen);
    if (ret != 0) {
        printf("ml-training(%s): error: error getting bytes from key: %s (bucket: %s)\n",
               us.c_str(),
               dataKey.c_str(),
               bucketName.c_str());
    }
    imageData.assign(keyBytes, keyBytes + keyBytesLen);
#else
    imageData = s3cli.getKeyBytes(bucketName, dataKey);
#endif

    std::vector<uint8_t> labelsData;
#ifdef __faasm
    keyBytes = nullptr;
    keyBytesLen = 0;
    ret =
      __faasm_s3_get_key_bytes(bucketName.c_str(), labelsKey.c_str(), &keyBytes, &keyBytesLen);
    if (ret != 0) {
        printf("ml-training(%s): error: error getting bytes from key: %s (bucket: %s)\n",
               us.c_str(),
               labelsKey.c_str(),
               bucketName.c_str());
    }
    labelsData.assign(keyBytes, keyBytes + keyBytesLen);
#else
    labelsData = s3cli.getKeyBytes(bucketName, labelsKey);
#endif

    cv::Mat data = deserializeMat(imageData);
    cv::Mat labels = deserializeMat(labelsData);

    // Train random forest
    std::cout << "ml-training(" << us << "): beginning to train rf..." << std::endl;
    cv::Ptr<cv::ml::RTrees> rf = cv::ml::RTrees::create();
        rf->setMaxDepth(10);
        rf->setMinSampleCount(5);
        rf->setRegressionAccuracy(0.01f);
        rf->setMaxCategories(15);
        rf->setTermCriteria(cv::TermCriteria(cv::TermCriteria::MAX_ITER, 100, 0.01f));
        rf->train(data, cv::ml::ROW_SAMPLE, labels);
    std::cout << "ml-training(" << us << "): done training!" << std::endl;

    // Serialize and upload to S3
    auto rfData = serializeForest(rf);

    // Upload the serialized results
    std::string modelDataKey = "ml-training/outputs/" + us;
#ifdef __faasm
    // Overwrite the results
    ret =
      __faasm_s3_add_key_bytes(bucketName.c_str(),
                               modelDataKey.c_str(),
                               rfData.data(),
                               rfData.size(),
                               true);
    if (ret != 0) {
        std::cerr << "ml-training(" << us << "): error uploading model data for inference" << std::endl;
        return 1;
    }
#else
    s3cli.addKeyBytes(bucketName, modelDataKey, rfData);
    s3::shutdownS3Wrapper();
#endif

    return 0;
}
