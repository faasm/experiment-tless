#include "S3Wrapper.hpp"

#include <aws/s3/S3Errors.h>
#include <aws/s3/model/CreateBucketRequest.h>
#include <aws/s3/model/DeleteBucketRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/ListObjectsRequest.h>
#include <aws/s3/model/PutObjectRequest.h>

using namespace Aws::S3::Model;
using namespace Aws::Client;
using namespace Aws::Auth;

namespace s3 {

static Aws::SDKOptions options;

template<typename R>
R reqFactory(const std::string& bucket)
{
    R req;
    req.SetBucket(bucket);
    return req;
}

template<typename R>
R reqFactory(const std::string& bucket, const std::string& key)
{
    R req = reqFactory<R>(bucket);
    req.SetKey(key);
    return req;
}

#define CHECK_ERRORS(response, bucketName, keyName)                            \
    {                                                                          \
        if (!response.IsSuccess()) {                                           \
            const auto& err = response.GetError();                             \
            if (std::string(bucketName).empty()) {                             \
                std::cerr << "General S3 error: " << bucketName << std::endl;  \
            } else if (std::string(keyName).empty()) {                         \
                std::cerr << "S3 error with bucket: "                          \
                          << bucketName << std::endl;                          \
            } else {                                                           \
                std::cerr << "S3 error with bucket/key:"                       \
                          << bucketName << "/" << keyName << std::endl;        \
            }                                                                  \
            std::cerr << "S3 error: "                                          \
                      << err.GetExceptionName().c_str()                        \
                      << err.GetMessage().c_str() << std::endl;                \
            throw std::runtime_error("S3 error");                              \
        }                                                                      \
    }

std::shared_ptr<AWSCredentialsProvider> getCredentialsProvider()
{
    return Aws::MakeShared<ProfileConfigFileAWSCredentialsProvider>("local");
}

ClientConfiguration getClientConf(long timeout)
{
    // There are a couple of conflicting pieces of info on how to configure
    // the AWS C++ SDK for use with minio:
    // https://stackoverflow.com/questions/47105289/how-to-override-endpoint-in-aws-sdk-cpp-to-connect-to-minio-server-at-localhost
    // https://github.com/aws/aws-sdk-cpp/issues/587
    ClientConfiguration config;

    char* s3Host = std::getenv("S3_HOST");
    if (s3Host == nullptr) {
        std::cerr << "s3-wrapper: error: S3_HOST env. var not set!"
                  << std::endl;
        throw std::runtime_error("S3 error");
    }
    std::string s3HostStr(s3Host);

    char* s3Port = std::getenv("S3_PORT");
    if (s3Port == nullptr) {
        std::cerr << "s3-wrapper: error: S3_PORT env. var not set!"
                  << std::endl;
        throw std::runtime_error("S3 error");
    }
    std::string s3PortStr(s3Port);

    config.region = "";
    config.verifySSL = false;
    config.endpointOverride = s3HostStr + ":" + s3PortStr;
    config.connectTimeoutMs = S3_CONNECT_TIMEOUT_MS;
    config.requestTimeoutMs = timeout;

    // Use HTTP, not HTTPS
    config.scheme = Aws::Http::Scheme::HTTP;

    return config;
}

void initS3Wrapper()
{
    char* s3Host = std::getenv("S3_HOST");
    if (s3Host == nullptr) {
        std::cerr << "s3-wrapper: error: S3_HOST env. var not set!"
                  << std::endl;
        throw std::runtime_error("S3 error");
    }
    std::string s3HostStr(s3Host);

    char* s3Port = std::getenv("S3_PORT");
    if (s3Port == nullptr) {
        std::cerr << "s3-wrapper: error: S3_PORT env. var not set!"
                  << std::endl;
        throw std::runtime_error("S3 error");
    }
    std::string s3PortStr(s3Port);

    std::cout << "s3-wrapper: initialising s3 setup at "
              << s3HostStr << ":" << s3PortStr << std::endl;

    Aws::InitAPI(options);

    char* s3Bucket = std::getenv("S3_BUCKET");
    if (s3Bucket == nullptr) {
        std::cerr << "s3-wrapper: error: S3_BUCKET env. var not set!"
                  << std::endl;
        throw std::runtime_error("S3 error");
    }
    std::string s3BucketStr(s3Bucket);

    S3Wrapper s3;
    s3.createBucket(s3BucketStr);

    // Check we can write/ read
    s3.addKeyStr(s3BucketStr, "ping", "pong");
    std::string response = s3.getKeyStr(s3BucketStr, "ping");
    if (response != "pong") {
        std::cerr << "s3-wrapper: unable to read/write from/to S3 "
                  << "(" << response << ")" << std::endl;
        throw std::runtime_error("S3 error");
    }

    std::cout << "s3-wrapper: succesfully pinged s3 at "
              << s3HostStr << ":" << s3PortStr << std::endl;
}

void shutdownS3Wrapper()
{
    Aws::ShutdownAPI(options);
}

S3Wrapper::S3Wrapper()
  : clientConf(getClientConf(S3_REQUEST_TIMEOUT_MS))
  , client(AWSCredentials(std::getenv("S3_USER"), std::getenv("S3_PASSWORD")),
           clientConf,
           AWSAuthV4Signer::PayloadSigningPolicy::Never,
           false)
{}

void S3Wrapper::createBucket(const std::string& bucketName)
{
    std::cout << "s3-wrapper: creating bucket " << bucketName << std::endl;

    auto request = reqFactory<CreateBucketRequest>(bucketName);
    auto response = client.CreateBucket(request);

    if (!response.IsSuccess()) {
        const auto& err = response.GetError();

        auto errType = err.GetErrorType();
        if (errType == Aws::S3::S3Errors::BUCKET_ALREADY_OWNED_BY_YOU ||
            errType == Aws::S3::S3Errors::BUCKET_ALREADY_EXISTS) {
            std::cout << "s3-wrapper: bucket already exists " << bucketName
                      << std::endl;
        } else {
            CHECK_ERRORS(response, bucketName, "");
        }
    }
}

void S3Wrapper::deleteBucket(const std::string& bucketName)
{
    std::cout << "s3-wrapper: deleting bucket " << bucketName << std::endl;

    auto request = reqFactory<DeleteBucketRequest>(bucketName);
    auto response = client.DeleteBucket(request);

    if (!response.IsSuccess()) {
        const auto& err = response.GetError();
        auto errType = err.GetErrorType();
        if (errType == Aws::S3::S3Errors::NO_SUCH_BUCKET) {
            std::cout << "s3-wrapper: bucket already deleted " << bucketName
                      << std::endl;
        } else if (err.GetExceptionName() == "BucketNotEmpty") {
            std::cout << "s3-wrapper: bucket not empty, deleting keys "
                      << bucketName << std::endl;

            std::vector<std::string> keys = listKeys(bucketName);
            for (const auto& k : keys) {
                deleteKey(bucketName, k);
            }

            // Recursively delete
            deleteBucket(bucketName);
        } else {
            CHECK_ERRORS(response, bucketName, "");
        }
    }
}

std::vector<std::string> S3Wrapper::listBuckets()
{
    auto response = client.ListBuckets();
    CHECK_ERRORS(response, "", "");

    Aws::Vector<Bucket> bucketObjects = response.GetResult().GetBuckets();

    std::vector<std::string> bucketNames;
    for (auto const& bucketObject : bucketObjects) {
        const Aws::String& awsStr = bucketObject.GetName();
        bucketNames.emplace_back(awsStr.c_str(), awsStr.size());
    }

    return bucketNames;
}

std::vector<std::string> S3Wrapper::listKeys(const std::string& bucketName, const std::string& prefix)
{
    auto request = reqFactory<ListObjectsRequest>(bucketName);
    if (!prefix.empty()) {
        request.SetPrefix(prefix);
    }
    auto response = client.ListObjects(request);

    std::vector<std::string> keys;
    if (!response.IsSuccess()) {
        const auto& err = response.GetError();
        auto errType = err.GetErrorType();

        if (errType == Aws::S3::S3Errors::NO_SUCH_BUCKET) {
            return keys;
        }

        CHECK_ERRORS(response, bucketName, "");
    }

    Aws::Vector<Object> keyObjects = response.GetResult().GetContents();
    if (keyObjects.empty()) {
        return keys;
    }

    for (auto const& keyObject : keyObjects) {
        const Aws::String& awsStr = keyObject.GetKey();
        keys.emplace_back(awsStr.c_str());
    }

    return keys;
}

void S3Wrapper::deleteKey(const std::string& bucketName,
                          const std::string& keyName)
{
    auto request = reqFactory<DeleteObjectRequest>(bucketName, keyName);
    auto response = client.DeleteObject(request);

    if (!response.IsSuccess()) {
        const auto& err = response.GetError();
        auto errType = err.GetErrorType();

        if (errType == Aws::S3::S3Errors::NO_SUCH_KEY) {
            std::cout << "s3-wrapper: key already deleted "
                      << bucketName << "/" << keyName << std::endl;
        } else if (errType == Aws::S3::S3Errors::NO_SUCH_BUCKET) {
            std::cout << "s3-wrapper: bucket already deleted "
                      << bucketName << "/" << keyName << std::endl;
        } else {
            CHECK_ERRORS(response, bucketName, keyName);
        }
    }
}

void S3Wrapper::addKeyBytes(const std::string& bucketName,
                            const std::string& keyName,
                            const std::vector<uint8_t>& data)
{
    // See example:
    // https://github.com/awsdocs/aws-doc-sdk-examples/blob/main/cpp/example_code/s3/put_object_buffer.cpp
    auto request = reqFactory<PutObjectRequest>(bucketName, keyName);

    const std::shared_ptr<Aws::IOStream> dataStream =
      Aws::MakeShared<Aws::StringStream>((char*)data.data());
    dataStream->write((char*)data.data(), data.size());
    dataStream->flush();

    request.SetBody(dataStream);

    auto response = client.PutObject(request);
    CHECK_ERRORS(response, bucketName, keyName);
}

void S3Wrapper::addKeyStr(const std::string& bucketName,
                          const std::string& keyName,
                          const std::string& data)
{
    // See example:
    // https://github.com/awsdocs/aws-doc-sdk-examples/blob/main/cpp/example_code/s3/put_object_buffer.cpp
    auto request = reqFactory<PutObjectRequest>(bucketName, keyName);

    const std::shared_ptr<Aws::IOStream> dataStream =
      Aws::MakeShared<Aws::StringStream>("");
    *dataStream << data;
    dataStream->flush();

    request.SetBody(dataStream);
    auto response = client.PutObject(request);
    CHECK_ERRORS(response, bucketName, keyName);
}

std::vector<uint8_t> S3Wrapper::getKeyBytes(const std::string& bucketName,
                                            const std::string& keyName,
                                            bool tolerateMissing)
{
    auto request = reqFactory<GetObjectRequest>(bucketName, keyName);
    GetObjectOutcome response = client.GetObject(request);

    if (!response.IsSuccess()) {
        const auto& err = response.GetError();
        auto errType = err.GetErrorType();

        if (tolerateMissing && (errType == Aws::S3::S3Errors::NO_SUCH_KEY)) {
            std::vector<uint8_t> empty;
            return empty;
        }

        CHECK_ERRORS(response, bucketName, keyName);
    }

    std::vector<uint8_t> rawData(response.GetResult().GetContentLength());
    response.GetResult().GetBody().read((char*)rawData.data(), rawData.size());
    return rawData;
}

std::string S3Wrapper::getKeyStr(const std::string& bucketName,
                                 const std::string& keyName)
{
    auto request = reqFactory<GetObjectRequest>(bucketName, keyName);
    GetObjectOutcome response = client.GetObject(request);
    CHECK_ERRORS(response, bucketName, keyName);

    std::ostringstream ss;
    auto* responseStream = response.GetResultWithOwnership().GetBody().rdbuf();
    ss << responseStream;

    return ss.str();
}
}
