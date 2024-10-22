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

cv::Ptr<cv::ml::RTrees> deserializeForest(const std::vector<uint8_t>& buffer) {
    std::string modelData(buffer.begin(), buffer.end());
    cv::FileStorage fs(modelData, cv::FileStorage::READ | cv::FileStorage::MEMORY);
    cv::Ptr<cv::ml::RTrees> forest = cv::ml::RTrees::create();
    forest->read(fs.root());

    return forest;
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
        std::cerr << "ml-infernce(" << us << "): error: error getting bytes from key: "
                  << s3file << " (bucket: " << bucketName << ")"
                  << std::endl;
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
            std::cout << "ml-inference(" << us << "): loaded " << label << "/" << numFiles << " images" << std::endl;
        }

        std::vector<uint8_t> imageContents;
#ifdef __faasm
        uint8_t* keyBytes;
        int keyBytesLen;

        int ret =
          __faasm_s3_get_key_bytes(bucketName.c_str(), image.c_str(), &keyBytes, &keyBytesLen);
        if (ret != 0) {
            std::cerr << "ml-infernce(" << us << "): error: error getting bytes from key: "
                      << image << " (bucket: " << bucketName << ")"
                      << std::endl;
        }

        imageContents.assign(keyBytes, keyBytes + keyBytesLen);
#else
        imageContents = s3cli.getKeyBytes(bucketName, image);
#endif

        cv::Mat img = cv::imdecode(imageContents, cv::IMREAD_GRAYSCALE);
        if (!img.empty()) {
            cv::resize(img, img, cv::Size(64, 64));
            data.push_back(img.reshape(1, 1));
            labels.push_back(label);
        }

        label++;
    }
}

void loadModelParts(const std::string& us,
                    const std::string& bucketName,
                    const std::string& modelDir,
                    std::vector<cv::Ptr<cv::ml::RTrees>>& forests)
{
    std::vector<std::string> s3files;
#ifdef __faasm
    int numKeys = __faasm_s3_get_num_keys_with_prefix(
      bucketName.c_str(), modelDir.c_str());

    char** keysBuffer = (char**) malloc(numKeys * sizeof(char*));
    int* keysBufferLens = (int*) malloc(numKeys * sizeof(int32_t));

    __faasm_s3_list_keys_with_prefix(
      bucketName.c_str(), modelDir.c_str(), keysBuffer, keysBufferLens);

    for (int i = 0; i < numKeys; i++) {
        std::string tmpString;
        tmpString.assign(keysBuffer[i], keysBuffer[i] + keysBufferLens[i]);
        s3files.push_back(tmpString);
    }

#else
    s3::S3Wrapper s3cli;
    s3files = s3cli.listKeys(bucketName, modelDir);
#endif

    // Download each file, and parse into a RF
    for (const auto& file : s3files) {
        std::vector<uint8_t> rfData;
#ifdef __faasm
        uint8_t* keyBytes;
        int keyBytesLen;

        int ret =
          __faasm_s3_get_key_bytes(bucketName.c_str(), file.c_str(), &keyBytes, &keyBytesLen);
        if (ret != 0) {
            std::cerr << "ml-inference(" << us << "): error: error getting bytes from key: "
                      << file << "(bucket: " << bucketName << ")"
                      << std::endl;
        }
        rfData = std::vector<uint8_t>(keyBytes, keyBytes + keyBytesLen);
#else
        rfData = s3cli.getKeyBytes(bucketName, file);
#endif

        auto forest = deserializeForest(rfData);
        forests.push_back(forest);
    }
}

int sanityCheckForests(const std::string& us,
                       const std::vector<cv::Ptr<cv::ml::RTrees>>& forests)
{
    // Sanity check the forests
    if (forests.empty()) {
        std::cerr << "ml-inference(" << us << "): forest deserialization failed or is empty!" << std::endl;
        return 1;
    }
    for (const auto& forest : forests) {
        if (!forest || forest->getRoots().empty()) {
            std::cerr << "ml-inference(" << us << "): forest deserialization failed or is empty!" << std::endl;
            return 1;
        }
    }
    for (const auto& forest : forests) {
        int treeCount = forest->getRoots().size();
        if (treeCount <= 0) {
            std::cerr << "ml-inference(" << us << "): error: no trees in the forest!" << std::endl;
            return 1;
        }

        // Optional: Check other parameters
        int maxDepth = forest->getMaxDepth();
        if (maxDepth <= 0) {
            std::cerr << "ml-inference(" << us << "): error: invalid max depth!" << std::endl;
            return 1;
        }
    }

    return 0;
}

float predictEnsemble(const std::vector<cv::Ptr<cv::ml::RTrees>>& forests, const cv::Mat& sample) {
    float aggregatedPrediction = 0.0f;
    for (const auto& forest : forests) {
        aggregatedPrediction += (int) forest->predict(sample);
    }

    return aggregatedPrediction / forests.size();
}

int main(int argc, char** argv) {
    std::string bucketName = "tless";
    int id;
    std::string modelDir;
    std::string dataKey;

#ifdef __faasm
    // Get the object key as an input
    int inputSize = faasmGetInputSize();
    char inputChar[inputSize];
    faasmGetInput((uint8_t*)inputChar, inputSize);

    std::string tmpStr(inputChar, inputChar + inputSize);
    auto parts = splitByDelimiter(tmpStr, ":");
    if (parts.size() != 3) {
        std::cerr << "ml-inference(predict): error parsing driver input" << std::endl;
        return 1;
    }

    id = std::stoi(parts.at(0));
    modelDir = parts.at(1);
    dataKey = parts.at(2);
#else
    if (argc != 4) {
        std::cerr << "ml-inference(predict): error parsing driver input" << std::endl;
        return 1;
    }

    id = std::stoi(argv[1]);
    modelDir = argv[2];
    dataKey = argv[3];

    s3::initS3Wrapper();
    s3::S3Wrapper s3cli;
#endif
    std::string us = "predict-" + std::to_string(id);

    if (!tless::checkChain("ml-inference", "predict", id)) {
        std::cerr << "ml-inference(" << us << "): error checking TLess chain" << std::endl;
        return 1;
    }

    std::cout << "ml-inference(" << us << "): predicting for images in "
              << dataKey
              << std::endl
              << "ml-inference(" << us << "): using model from "
              << modelDir
              << std::endl;

    // First, load all image data
    std::cout << "ml-inference(" << us << "): beginning to load images..." << std::endl;
    std::vector<cv::Mat> images;
    std::vector<int> labels;
    loadImages(us, bucketName, dataKey, images, labels);
    int numImages = images.size();
    std::cout << "ml-inference(" << us << "): " << images.size() << " images loaded!" << std::endl;

    // Convert data to a single matrix
    std::cout << "ml-inference(" << us << "): converting data..." << std::endl;
    cv::Mat data;
    cv::vconcat(images, data);
    data.convertTo(data, CV_32F);
    std::cout << "ml-inference(" << us << "): data converted" << std::endl;

    // Perform PCA with 10 principal components
    std::cout << "ml-inference(" << us << "): performing PCA analysis..." << std::endl;
    cv::PCA pca(data, cv::Mat(), cv::PCA::DATA_AS_ROW, 10);
    cv::Mat pcaResult;
    pca.project(data, pcaResult);

    // Prepare labels for training
    cv::Mat labelsMat(labels);
    labelsMat.convertTo(labelsMat, CV_32S);
    std::cout << "ml-inference(" << us << "): PCA on images succeded!" << std::endl;

    // Second, load all model data
    std::cout << "ml-inference(" << us << "): beginning to load model..." << std::endl;
    std::vector<cv::Ptr<cv::ml::RTrees>> forests;
    loadModelParts(us, bucketName, modelDir, forests);
    std::cout << "ml-inference(" << us << "): model loaded!" << std::endl;

    // Sanitiy check forests
    if (sanityCheckForests(us, forests) != 0) {
        std::cerr << "ml-inference(" << us << "): error checking forests" << std::endl;
        return 1;
    }

    // Third, perform inference
    std::cout << "ml-inference(" << us << "): beginning to perform inference on " << numImages << " images..." << std::endl;
    std::vector<float> inferenceResults;
    for (int i = 0; i < numImages; i++) {
        inferenceResults.push_back(predictEnsemble(forests, pcaResult.row(i)));
    }
    std::cout << "ml-inference(" << us << "): inference done!" << std::endl;

    // Serialize results and upload to S3
    std::string inferenceResultsStr;
    for (int i = 0; i < inferenceResults.size(); i++) {
        inferenceResultsStr += std::to_string(i) + "," + std::to_string(inferenceResults.at(i)) + ",";
    }

    // Upload the serialized results
    std::string resultsKey = "ml-inference/outputs/" + us;
#ifdef __faasm
    int ret =
      __faasm_s3_add_key_bytes(bucketName.c_str(),
                               resultsKey.c_str(),
                               (uint8_t*) inferenceResultsStr.c_str(),
                               inferenceResultsStr.size(),
                               true);
    if (ret != 0) {
        std::cerr << "ml-inference(" << us << "): error uploading inference results!" << std::endl;
        return 1;
    }
#else
    s3cli.addKeyStr(bucketName, resultsKey, inferenceResultsStr);
    s3::shutdownS3Wrapper();
#endif

    return 0;
}
