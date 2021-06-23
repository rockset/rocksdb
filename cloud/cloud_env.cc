// Copyright (c) 2017 Rockset.
#ifndef ROCKSDB_LITE

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif

#include "cloud/aws/aws_env.h"
#include "cloud/cloud_env_impl.h"
#include "cloud/cloud_env_wrapper.h"
#include "cloud/db_cloud_impl.h"
#include "cloud/filename.h"
#include "file/filename.h"
#include "port/likely.h"
#include "rocksdb/cloud/cloud_log_controller.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/options.h"
#include "rocksdb/status.h"

namespace ROCKSDB_NAMESPACE {

bool CloudEnvOptions::GetNameFromEnvironment(const char* name, const char* alt,
                                             std::string* result) {
  char* value = getenv(name);  // See if name is set in the environment
  if (value == nullptr &&
      alt != nullptr) {   // Not set.  Do we have an alt name?
    value = getenv(alt);  // See if alt is in the environment
  }
  if (value != nullptr) {   // Did we find the either name/alt in the env?
    result->assign(value);  // Yes, update result
    return true;            // And return success
  } else {
    return false;  // No, return not found
  }
}
void CloudEnvOptions::TEST_Initialize(const std::string& bucket,
                                      const std::string& object,
                                      const std::string& region) {
  src_bucket.TEST_Initialize(bucket, object, region);
  dest_bucket = src_bucket;
  credentials.TEST_Initialize();
}

BucketOptions::BucketOptions() { prefix_ = "rockset."; }

void BucketOptions::SetBucketName(const std::string& bucket,
                                  const std::string& prefix) {
  if (!prefix.empty()) {
    prefix_ = prefix;
  }

  bucket_ = bucket;
  if (bucket_.empty()) {
    name_.clear();
  } else {
    name_ = prefix_ + bucket_;
  }
}

static Status IsNormalizedPath(const std::string& path) {
  auto pos = path.find_first_of("\\{}[]<>%~#|^\'\"");
  if (pos != std::string::npos) {
    return Status::InvalidArgument("Illegal character in object path:", path);
  }
  pos = path.find_first_of("&$@=;:+,?");
  if (pos != std::string::npos) {
    return Status::InvalidArgument("Special character in object path: ", path);
  }
  return Status::OK();
}

Status BucketOptions::NormalizeObjectPath(const std::string& path,
                                          std::string* result) {
  // Remove the drive if there is one...
  auto colon = path.find(':');
  std::string normalized;
  if (colon != std::string::npos) {
    normalized = path.substr(colon + 1);
  } else {
    normalized = path;
  }
  // Replace any "\" with "/"
  for (auto pos = normalized.find('\\'); pos != std::string::npos;
       pos = normalized.find('\\', pos)) {
    normalized[pos] = '/';
  }
  // Remove any duplicate markers
  normalized = NormalizePath(normalized);
  Status s = IsNormalizedPath(normalized);
  if (s.ok()) {
    *result = normalized;
  }
  return s;
}

Status BucketOptions::SetObjectPath(const std::string& object) {
  Status s = IsNormalizedPath(object);
  if (s.ok()) {
    object_ = object;
  }
  return s;
}

// Initializes the bucket properties

void BucketOptions::TEST_Initialize(const std::string& bucket,
                                    const std::string& object,
                                    const std::string& region) {
  std::string prefix;
  // If the bucket name is not set, then the bucket name is not set,
  // Set it to either the value of the environment variable or geteuid
  if (!CloudEnvOptions::GetNameFromEnvironment("ROCKSDB_CLOUD_TEST_BUCKET_NAME",
                                               "ROCKSDB_CLOUD_BUCKET_NAME",
                                               &bucket_)) {
#ifdef _WIN32
    char user_name[257];  // UNLEN + 1
    DWORD dwsize = sizeof(user_name);
    if (!::GetUserName(user_name, &dwsize)) {
      bucket_ = bucket_ + "unknown";
    } else {
      bucket_ = bucket_ + user_name;
    }
#else
    bucket_ = bucket + std::to_string(geteuid());
#endif
  }
  if (CloudEnvOptions::GetNameFromEnvironment(
          "ROCKSDB_CLOUD_TEST_BUCKET_PREFIX", "ROCKSDB_CLOUD_BUCKET_PREFIX",
          &prefix)) {
    prefix_ = prefix;
  }
  name_ = prefix_ + bucket_;
  std::string value;
  if (CloudEnvOptions::GetNameFromEnvironment("ROCKSDB_CLOUD_TEST_OBECT_PATH",
                                              "ROCKSDB_CLOUD_OBJECT_PATH",
                                              &value)) {
    NormalizeObjectPath(value, &value);
  } else {
    NormalizeObjectPath(object, &value);
  }
  SetObjectPath(value);

  if (!CloudEnvOptions::GetNameFromEnvironment(
          "ROCKSDB_CLOUD_TEST_REGION", "ROCKSDB_CLOUD_REGION", &region_)) {
    region_ = region;
  }
}

CloudEnv::CloudEnv(const CloudEnvOptions& options, Env* base,
                   const std::shared_ptr<Logger>& logger)
    : cloud_env_options(options), base_env_(base), info_log_(logger) {}

CloudEnv::~CloudEnv() {
  cloud_env_options.cloud_log_controller.reset();
  cloud_env_options.storage_provider.reset();
}

Status CloudEnv::NewAwsEnv(
    Env* base_env, const std::string& src_cloud_bucket,
    const std::string& src_cloud_object, const std::string& src_cloud_region,
    const std::string& dest_cloud_bucket, const std::string& dest_cloud_object,
    const std::string& dest_cloud_region, const CloudEnvOptions& cloud_options,
    const std::shared_ptr<Logger>& logger, CloudEnv** cenv) {
  CloudEnvOptions options = cloud_options;
  Status s;
  if (!src_cloud_bucket.empty())
    options.src_bucket.SetBucketName(src_cloud_bucket);
  if (!src_cloud_object.empty()) {
    s = options.src_bucket.SetObjectPath(src_cloud_object);
    if (!s.ok()) {
      return s;
    }
  }
  if (!src_cloud_region.empty()) options.src_bucket.SetRegion(src_cloud_region);
  if (!dest_cloud_bucket.empty()) 
    options.dest_bucket.SetBucketName(dest_cloud_bucket);
  if (!dest_cloud_object.empty()) {
    s = options.dest_bucket.SetObjectPath(dest_cloud_object);
    if (!s.ok()) {
      return s;
    }
  }
  if (!dest_cloud_region.empty())
    options.dest_bucket.SetRegion(dest_cloud_region);
  return NewAwsEnv(base_env, options, logger, cenv);
}

#ifndef USE_AWS
Status CloudEnv::NewAwsEnv(Env* /*base_env*/,
                           const CloudEnvOptions& /*options*/,
                           const std::shared_ptr<Logger>& /*logger*/,
                           CloudEnv** /*cenv*/) {
  return Status::NotSupported("RocksDB Cloud not compiled with AWS support");
}
#else
Status CloudEnv::NewAwsEnv(Env* base_env, const CloudEnvOptions& options,
                           const std::shared_ptr<Logger>& logger,
                           CloudEnv** cenv) {
  // Dump out cloud env options
  options.Dump(logger.get());

  Status st = AwsEnv::NewAwsEnv(base_env, options, logger, cenv);
  if (st.ok()) {
    // store a copy of the logger
    CloudEnvImpl* cloud = static_cast<CloudEnvImpl*>(*cenv);
    cloud->info_log_ = logger;

    // start the purge thread only if there is a destination bucket
    if (options.dest_bucket.IsValid() && options.run_purger) {
      cloud->purge_thread_ = std::thread([cloud] { cloud->Purger(); });
    }
  }
  return st;
}
#endif

}  // namespace ROCKSDB_NAMESPACE
#endif  // ROCKSDB_LITE
