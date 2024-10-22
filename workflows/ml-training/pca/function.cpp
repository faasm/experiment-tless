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

void loadImages(const std::string& us,
                const std::string& bucketName,
                const std::string& s3file,
                std::vector<cv::Mat>& data,
                std::vector<int>& labels)
{
    std::string imageNames;
#ifdef __faasm
    uint8_t* keyBytes;
    int keyBytesLen;

    int ret =
      __faasm_s3_get_key_bytes(bucketName.c_str(), s3file.c_str(), &keyBytes, &keyBytesLen);
    if (ret != 0) {
        printf("ml-training(%s): error: error getting bytes from key: %s (bucket: %s)\n",
               us.c_str(),
               s3file.c_str(),
               bucketName.c_str());
    }

    imageNames.assign((char*) keyBytes, (char*) keyBytes + keyBytesLen);
#else
    s3::S3Wrapper s3cli;
    imageNames = s3cli.getKeyStr(bucketName, s3file);
#endif

    std::istringstream ss(imageNames);
    std::string image;
    int numFiles = 0;
    while (std::getline(ss, image, ',')) {
        numFiles += 1;
    }

    ss.clear();
    ss.seekg(0, std::ios::beg);
    int label = 0;
    int progressEvery = numFiles / 5;
    while (std::getline(ss, image, ',')) {
        if ((label % progressEvery) == 0) {
            std::cout << "ml-training(" << us << "): loaded " << label << "/" << numFiles << " images" << std::endl;
        }

        std::vector<uint8_t> imageContents;
#ifdef __faasm
        uint8_t* keyBytes;
        int keyBytesLen;

        int ret =
          __faasm_s3_get_key_bytes(bucketName.c_str(), image.c_str(), &keyBytes, &keyBytesLen);
        if (ret != 0) {
            printf("ml-training(%s): error: error getting bytes from key: %s (bucket: %s)\n",
                   us.c_str(),
                   image.c_str(),
                   bucketName.c_str());
        }

        imageContents.assign(keyBytes, keyBytes + keyBytesLen);
#else
        imageContents = s3cli.getKeyBytes(bucketName, image);
#endif

        // TODO: consider resizing instead of push_back
        cv::Mat img = cv::imdecode(imageContents, cv::IMREAD_GRAYSCALE);
        if (!img.empty()) {
            cv::resize(img, img, cv::Size(64, 64));
            data.push_back(img.reshape(1, 1));
            labels.push_back(label);
        }

        label++;
    }
}

std::vector<uint8_t> serializeMat(const cv::Mat& mat) {
    std::vector<uint8_t> buffer;

    int header[3] = { mat.rows, mat.cols, mat.type() };
    buffer.insert(buffer.end(), reinterpret_cast<uint8_t*>(header), reinterpret_cast<uint8_t*>(header + 3));

    size_t dataSize = mat.total() * mat.elemSize();
    const uint8_t* dataPtr = mat.ptr<uint8_t>();
    buffer.insert(buffer.end(), dataPtr, dataPtr + dataSize);

    return buffer;
}

// Function to split and serialize a Mat into N parts
std::vector<std::vector<uint8_t>> splitAndSerialize(const cv::Mat& mat, int numMats) {
    std::vector<std::vector<uint8_t>> serializedParts;
    int rowsPerPart = mat.rows / numMats;

    for (int i = 0; i < numMats; ++i) {
        int startRow = i * rowsPerPart;
        int endRow = (i == numMats - 1) ? mat.rows : (i + 1) * rowsPerPart;

        // Create a submatrix
        cv::Mat part = mat(cv::Range(startRow, endRow), cv::Range::all());
        // Serialize the submatrix
        serializedParts.push_back(serializeMat(part));
    }

    return serializedParts;
}

/*
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
*/

int main(int argc, char** argv) {
    std::string bucketName = "tless";
    std::string s3dir;
    int numTrainFuncs;
    int id;

#ifdef __faasm
    // Get the object key as an input
    int inputSize = faasmGetInputSize();
    char inputChar[inputSize];
    faasmGetInput((uint8_t*)inputChar, inputSize);

    std::string tmpStr(inputChar, inputChar + inputSize);
    auto parts = splitByDelimiter(tmpStr, ":");
    if (parts.size() != 3) {
        std::cerr << "ml-training(pca): error parsing partition input" << std::endl;
        return 1;
    }

    id = std::stoi(parts.at(0));
    s3dir = parts.at(1);
    numTrainFuncs = std::stoi(parts.at(2));
#else
    if (argc != 4) {
        std::cerr << "ml-training(pca): error parsing driver input" << std::endl;
        return 1;
    }
    id = std::stoi(argv[1]);
    s3dir = argv[2];
    numTrainFuncs = std::stoi(argv[3]);

    s3::initS3Wrapper();
    s3::S3Wrapper s3cli;
#endif
    std::string us = "pca-" + std::to_string(id);

    if (!tless::checkChain("ml-training", "pca", id)) {
        std::cerr << "ml-training(" << us << "): error checking TLess chain" << std::endl;
        return 1;
    }

    std::cout << "ml-training(" << us << "): running PCA on S3 dir "
              << s3dir
              << std::endl
              << "ml-training(" << us << "): chaining into "
              << numTrainFuncs
              << " parallel random forest trees"
              << std::endl;

    // Load images
    std::cout << "ml-training(" << us << "): beginning to load images..." << std::endl;
    std::vector<cv::Mat> images;
    std::vector<int> labels;
    loadImages(us, bucketName, s3dir, images, labels);
    std::cout << "ml-training(" << us << "): " << images.size() << " images loaded!" << std::endl;

    // Convert data to a single matrix
    std::cout << "ml-training(" << us << "): converting data..." << std::endl;
    cv::Mat data;
    cv::vconcat(images, data);
    data.convertTo(data, CV_32F);
    std::cout << "ml-training(" << us << "): data converted" << std::endl;

    // Perform PCA with 10 principal components
    std::cout << "ml-training(" << us << "): performing PCA analysis..." << std::endl;
    cv::PCA pca(data, cv::Mat(), cv::PCA::DATA_AS_ROW, 10);
    cv::Mat pcaResult;
    pca.project(data, pcaResult);

    // Prepare labels for training
    cv::Mat labelsMat(labels);
    labelsMat.convertTo(labelsMat, CV_32S);
    std::cout << "ml-training(" << us << "): PCA on images succeded!" << std::endl;

    // Split and invoke training
    std::cout << "ml-training(" << us << "): splitting and serializing results..." << std::endl;
    auto serializedMats = splitAndSerialize(pcaResult, numTrainFuncs);
    auto serializedLabels = splitAndSerialize(labelsMat, numTrainFuncs);
    std::cout << "ml-training(" << us << "): splitting and serializing done!" << std::endl;

    // Upload the serialized results
    for (int i = 0; i < serializedMats.size(); i++) {
        std::string dataKey = "ml-training/outputs/" + us + "/rf-" + std::to_string(i) + "-data";
#ifdef __faasm
        // Overwrite the results
        int ret =
          __faasm_s3_add_key_bytes(bucketName.c_str(),
                                   dataKey.c_str(),
                                   serializedMats.at(i).data(),
                                   serializedMats.at(i).size(),
                                   true);
        if (ret != 0) {
            std::cerr << "ml-training(" << us << "): error uploading PCA data for training" << std::endl;
            return 1;
        }
#else
        s3cli.addKeyBytes(bucketName, dataKey, serializedMats.at(i));
#endif

        // Upload the labels
        std::string labelsKey = "ml-training/outputs/" + us + "/rf-" + std::to_string(i) + "-labels";
#ifdef __faasm
        // Overwrite the results
        ret =
          __faasm_s3_add_key_bytes(bucketName.c_str(),
                                   labelsKey.c_str(),
                                   serializedLabels.at(i).data(),
                                   serializedLabels.at(i).size(),
                                   true);
        if (ret != 0) {
            std::cerr << "ml-training(" << us << "): error uploading labels data for training" << std::endl;
            return 1;
        }
#else
        s3cli.addKeyBytes(bucketName, labelsKey, serializedLabels.at(i));
#endif
    }

    std::vector<std::string> trainFuncIds;
    for (int i = 0; i < numTrainFuncs; i++) {
        std::string dataKey = "ml-training/outputs/" + us + "/rf-" + std::to_string(i) + "-data";
        std::string labelsKey = "ml-training/outputs/" + us + "/rf-" + std::to_string(i) + "-labels";
        std::string pcaInput = std::to_string(id) + ":" + std::to_string(i) + ":" + dataKey + ":" + labelsKey;
#ifdef __faasm
        // int pcaId = faasmChainNamed("rf", (uint8_t*) pcaInput.c_str(), pcaInput.size());
        int pcaId = tless::chain("ml-training", "pca", id, "rf", i, pcaInput);
        trainFuncIds.push_back(std::to_string(pcaId));
#endif
    }

    // Tell the driver the ids of the PCA funcs to wait on them
#ifdef __faasm
    std::string outputStr = join(trainFuncIds, ",");
    faasmSetOutput(outputStr.c_str(), outputStr.size());
#else
    s3::shutdownS3Wrapper();
#endif

    return 0;
}
