//  Copyright (c) 2016-present, Rockset, Inc.  All rights reserved.
//
#pragma once
#include "rocksdb/env.h"
#include "rocksdb/status.h"

#include <functional>
#include <memory>

namespace rocksdb {

class BucketObjectMetadata;

enum CloudType : unsigned char {
  kNone = 0x0,       // Not really a cloud env
  kAws = 0x1,        // AWS
  kGoogle = 0x2,     // Google
  kAzure = 0x3,      // Microsoft Azure
  kRackspace = 0x4,  // Rackspace
  kEnd = 0x5,
};

// Credentials needed to access cloud service
class CloudAccessCredentials {
 public:
  std::string access_key_id;
  std::string secret_key;
};

enum class CloudRequestOpType {
  kReadOp,
  kWriteOp,
  kListOp,
  kCreateOp,
  kDeleteOp,
  kCopyOp,
  kInfoOp
};

using CloudRequestCallback =
    std::function<void(CloudRequestOpType, uint64_t, uint64_t, bool)>;

class CloudStatistics;

//
// The cloud environment for rocksdb. It allows configuring the rocksdb
// Environent used for the cloud.
//
class CloudEnvOptions {
 public:
  // Specify the type of cloud-service to use.
  CloudType cloud_type;

  // Access credentials
  CloudAccessCredentials credentials;

  //
  // If true,  then sst files are stored locally and uploaded to the cloud in
  // the background. On restart, all files from the cloud that are not present
  // locally are downloaded.
  // If false, then local sst files are created, uploaded to cloud immediately,
  //           and local file is deleted. All reads are satisfied by fetching
  //           data from the cloud.
  // Default:  false
  bool keep_local_sst_files;

  // If true,  then .log and MANIFEST files are stored in a local file system.
  //           they are not uploaded to any cloud logging system.
  // If false, then .log and MANIFEST files are not stored locally, and are
  //           stored in a cloud-logging system like Kafka or Kinesis.
  // Default:  true
  bool keep_local_log_files;

  // The periodicity when the manifest should be made durable by backing it
  // to cloud store. If set to 0, then manifest is not uploaded to S3.
  // This feature is enabled only if keep_local_log_files = true.
  // Default:  1 minute
  uint64_t manifest_durable_periodicity_millis;

  // The time period when the purger checks and deleted obselete files.
  // This is the time when the purger wakes up, scans the cloud bucket
  // for files that are not part of any DB and then deletes them.
  // Default: 10 minutes
  uint64_t purger_periodicity_millis;

  // if non-null, will be called *after* every cloud operation with some basic
  // information about the operation. Use this to instrument your calls to the
  // cloud.
  // parameters: (op, size, latency in microseconds, is_success)
  std::shared_ptr<CloudRequestCallback> cloud_request_callback;

  // If non-null, then we should collect metrics about cloud environment operations
  std::shared_ptr<CloudStatistics> cloud_statistics;

  CloudEnvOptions(
      CloudType _cloud_type = CloudType::kAws,
      bool _keep_local_sst_files = false, bool _keep_local_log_files = true,
      uint64_t _manifest_durable_periodicity_millis = 60 * 1000,
      uint64_t _purger_periodicity_millis = 10 * 60 * 1000,
      std::shared_ptr<CloudRequestCallback> _cloud_request_callback = nullptr,
      std::shared_ptr<CloudStatistics> _cloud_statistics = nullptr)
      : cloud_type(_cloud_type),
        keep_local_sst_files(_keep_local_sst_files),
        keep_local_log_files(_keep_local_log_files),
        manifest_durable_periodicity_millis(
            _manifest_durable_periodicity_millis),
        purger_periodicity_millis(_purger_periodicity_millis),
        cloud_request_callback(_cloud_request_callback),
        cloud_statistics(_cloud_statistics) {
    assert(manifest_durable_periodicity_millis == 0 ||
           keep_local_log_files == true);
  }

  // print out all options to the log
  void Dump(Logger* log) const;
};

// A map of dbid to the pathname where the db is stored
typedef std::map<std::string, std::string> DbidList;

//
// The Cloud environment
//
class CloudEnv : public Env {
 public:
  // Returns the underlying env
  virtual Env* GetBaseEnv() = 0;
  virtual ~CloudEnv();

  // Empties all contents of the associated cloud storage bucket.
  virtual Status EmptyBucket(const std::string& bucket_prefix) = 0;

  // Reads a file from the cloud
  virtual Status NewSequentialFileCloud(const std::string& bucket_prefix,
                                        const std::string& fname,
                                        unique_ptr<SequentialFile>* result,
                                        const EnvOptions& options) = 0;

  // Saves and retrieves the dbid->dirname mapping in cloud storage
  virtual Status SaveDbid(const std::string& dbid,
                          const std::string& dirname) = 0;
  virtual Status GetPathForDbid(const std::string& bucket_prefix,
                                const std::string& dbid,
                                std::string* dirname) = 0;
  virtual Status GetDbidList(const std::string& bucket_prefix,
                             DbidList* dblist) = 0;
  virtual Status DeleteDbid(const std::string& bucket_prefix,
                            const std::string& dbid) = 0;

  // The SrcBucketPrefix identifies the cloud storage bucket and
  // GetSrcObjectPrefix specifies the path inside that bucket
  // where data files reside. The specified bucket is used in
  // a readonly mode by the associated DBCloud instance.
  virtual const std::string& GetSrcBucketPrefix() = 0;
  virtual const std::string& GetSrcObjectPrefix() = 0;

  // The DestBucketPrefix identifies the cloud storage bucket and
  // GetDestObjectPrefix specifies the path inside that bucket
  // where data files reside. The associated DBCloud instance
  // writes newly created files to this bucket.
  virtual const std::string& GetDestBucketPrefix() = 0;
  virtual const std::string& GetDestObjectPrefix() = 0;

  // returns the options used to create this env
  virtual const CloudEnvOptions& GetCloudEnvOptions() = 0;

  // returns all the objects that have the specified path prefix and
  // are stored in a cloud bucket
  virtual Status ListObjects(const std::string& bucket_name_prefix,
                             const std::string& bucket_object_prefix,
                             BucketObjectMetadata* meta) = 0;

  // Delete the specified object from the specified cloud bucket
  virtual Status DeleteObject(const std::string& bucket_name_prefix,
                              const std::string& bucket_object_path) = 0;

  // Does the specified object exist in the cloud storage
  virtual Status ExistsObject(const std::string& bucket_name_prefix,
                              const std::string& bucket_object_path) = 0;

  // Get the size of the object in cloud storage
  virtual Status GetObjectSize(const std::string& bucket_name_prefix,
                              const std::string& bucket_object_path,
                              size_t* filesize) = 0;

  // Copy the specified cloud object from one location in the cloud
  // storage to another location in cloud storage
  virtual Status CopyObject(const std::string& bucket_name_prefix_src,
                            const std::string& bucket_object_path_src,
                            const std::string& bucket_name_prefix_dest,
                            const std::string& bucket_object_path_dest) = 0;

  // Create a new AWS env.
  // src_bucket_name: bucket name suffix where db data is read from
  // src_object_prefix: all db objects in source bucket are prepended with this
  // dest_bucket_name: bucket name suffix where db data is written to
  // dest_object_prefix: all db objects in destination bucket are prepended with
  // this
  //
  // If src_bucket_name is empty, then the associated db does not read any
  // data from cloud storage.
  // If dest_bucket_name is empty, then the associated db does not write any
  // data to cloud storage.
  static Status NewAwsEnv(Env* base_env, const std::string& src_bucket_name,
                          const std::string& src_object_prefix,
                          const std::string& src_bucket_region,
                          const std::string& dest_bucket_name,
                          const std::string& dest_object_prefix,
                          const std::string& dest_bucket_region,
                          const CloudEnvOptions& env_options,
                          std::shared_ptr<Logger> logger, CloudEnv** cenv);
};

/*
 * The information about all objects stored in a cloud bucket
 */
class BucketObjectMetadata {
 public:
  // list of all pathnames
  std::vector<std::string> pathnames;
};

}  // namespace
