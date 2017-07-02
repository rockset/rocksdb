//  Copyright (c) 2016-present, Rockset, Inc.  All rights reserved.
//
// This file defines an AWS-S3 environment for rocksdb.
// A directory maps to an an zero-size object in an S3 bucket
// A sst file maps to an object in that S3 bucket.
//
#ifdef USE_AWS

#include <assert.h>
#include <fstream>
#include <iostream>

#include "cloud/aws/aws_env.h"
#include "cloud/aws/aws_file.h"
#include "rocksdb/cloud/cloud_statistics.h"
#include "util/coding.h"
#include "util/stderr_logger.h"
#include "util/string_util.h"

namespace rocksdb {

/******************** Readablefile ******************/

S3ReadableFile::S3ReadableFile(AwsEnv* env, const std::string& bucket_prefix,
                               const std::string& fname, bool is_file)
    : env_(env),
      fname_(fname),
      file_number_(0),
      offset_(0),
      file_size_(0),
      last_mod_time_(0),
      is_file_(is_file) {
  Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
      "[s3] S3ReadableFile opening file %s", fname_.c_str());
  assert(!is_file_ || IsSstFile(fname) || IsManifestFile(fname) ||
         IsIdentityFile(fname));
  s3_bucket_ = GetBucket(bucket_prefix);
  s3_object_ = Aws::String(fname_.c_str(), fname_.size());

  ParseFileName(basename(fname), &file_number_, &file_type_, &log_type_);

  // fetch file size from S3
  status_ = GetFileInfo();
}

S3ReadableFile::~S3ReadableFile() {
  Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
      "[s3] S3ReadableFile closed file %s", fname_.c_str());
  offset_ = 0;
}

// sequential access, read data at current offset in file
Status S3ReadableFile::Read(size_t n, Slice* result, char* scratch) {
  Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
      "[s3] S3ReadableFile reading %s %ld", fname_.c_str(), n);
  Status s = Read(offset_, n, result, scratch);

  // If the read successfully returned some data, then update
  // offset_
  if (s.ok()) {
    offset_ += result->size();
  }
  return s;
}

// random access, read data from specified offset in file
Status S3ReadableFile::Read(uint64_t offset, size_t n, Slice* result,
                            char* scratch) const {
  Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
      "[s3] S3ReadableFile reading %s at offset %ld size %ld", fname_.c_str(),
      offset, n);

  if (!status_.ok()) {
    return status_;
  }
  *result = Slice();

  if (offset >= file_size_) {
    Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
        "[s3] S3ReadableFile reading %s at offset %ld filesize %ld."
        " Nothing to do",
        fname_.c_str(), offset, file_size_);
    return Status::OK();
  }

  // trim size if needed
  if (offset + n > file_size_) {
    n = file_size_ - offset;
    Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
        "[s3] S3ReadableFile reading %s at offset %ld trimmed size %ld",
        fname_.c_str(), offset, n);
  }

  // create a range read request
  // Ranges are inclusive, so we can't read 0 bytes; read 1 instead and
  // drop it later.
  size_t rangeLen = (n != 0 ? n : 1);
  char buffer[512];
  int ret =
      snprintf(buffer, sizeof(buffer), "bytes=%ld-%ld", offset,
               offset + rangeLen - 1);
  if (ret < 0) {
    Log(InfoLogLevel::ERROR_LEVEL, env_->info_log_,
        "[s3] S3ReadableFile vsnprintf error %s offset %ld rangelen %ld\n",
        fname_.c_str(), offset, rangeLen);
    return Status::IOError("S3ReadableFile vsnprintf ", fname_.c_str());
  }
  Aws::String range(buffer);

  // set up S3 request to read this range
  Aws::S3::Model::GetObjectRequest request;
  request.SetBucket(s3_bucket_);
  request.SetKey(s3_object_);
  request.SetRange(range);

  Aws::S3::Model::GetObjectOutcome outcome =
      env_->s3client_->GetObject(request);
  bool isSuccess = outcome.IsSuccess();
  if (!isSuccess) {
    const Aws::Client::AWSError<Aws::S3::S3Errors>& error = outcome.GetError();
    std::string errmsg(error.GetMessage().c_str(), error.GetMessage().size());
    Aws::S3::S3Errors s3err = error.GetErrorType();
    if (s3err == Aws::S3::S3Errors::NO_SUCH_BUCKET ||
        s3err == Aws::S3::S3Errors::NO_SUCH_KEY ||
        s3err == Aws::S3::S3Errors::RESOURCE_NOT_FOUND ||
        errmsg.find("Response code: 404") != std::string::npos) {
      Log(InfoLogLevel::ERROR_LEVEL, env_->info_log_,
          "[s3] S3ReadableFile error in reading not-existent %s %s",
          fname_.c_str(), errmsg.c_str());
      return Status::NotFound(fname_, errmsg.c_str());
    }
    Log(InfoLogLevel::ERROR_LEVEL, env_->info_log_,
        "[s3] S3ReadableFile error in reading %s %ld %s %s", fname_.c_str(),
        offset, buffer, error.GetMessage().c_str());
    return Status::IOError(fname_, errmsg.c_str());
  }
  std::stringstream ss;
  // const Aws::S3::Model::GetObjectResult& res = outcome.GetResult();

  // extract data payload
  Aws::IOStream& body = outcome.GetResult().GetBody();
  uint64_t size = 0;
  if (n != 0) {
    body.read(scratch, n);
    size = body.gcount();
    assert(size <= n);
  }
  *result = Slice(scratch, size);

  Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
      "[s3] S3ReadableFile file %s filesize %ld read %d bytes", fname_.c_str(),
      file_size_, size);
  return Status::OK();
}

Status S3ReadableFile::Skip(uint64_t n) {
  Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
      "[s3] S3ReadableFile file %s skip %ld", fname_.c_str(), n);
  if (!status_.ok()) {
    return status_;
  }

  // Update offset_ so that it does not go beyond filesize
  offset_ += n;
  if (offset_ > file_size_) {
    offset_ = file_size_;
  }
  return Status::OK();
}

size_t S3ReadableFile::GetUniqueId(char* id, size_t max_size) const {
  // If this is an SST file name, then it can part of the persistent cache.
  // We need to generate a unique id for the cache.
  // If it is not a sst file, then nobody should be using this id.
  if (max_size >= sizeof(file_number_)) {
    char* rid = id;
    rid = EncodeVarint64(rid, file_number_);
    return static_cast<size_t>(rid - id);
  }
  return 0;
}

//
// Retrieves the metadata of file by making a HeadObject call to S3
//
Status S3ReadableFile::GetFileInfo() {
  Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_, "[s3] S3GetFileInfo %s",
      fname_.c_str());

  // set up S3 request to read the head
  Aws::S3::Model::HeadObjectRequest request;
  request.SetBucket(s3_bucket_);
  request.SetKey(s3_object_);

  Aws::S3::Model::HeadObjectOutcome outcome =
      env_->s3client_->HeadObject(request);
  bool isSuccess = outcome.IsSuccess();
  if (!isSuccess) {
    const Aws::Client::AWSError<Aws::S3::S3Errors>& error = outcome.GetError();
    std::string errmsg(error.GetMessage().c_str(), error.GetMessage().size());
    Aws::S3::S3Errors s3err = error.GetErrorType();

    if (s3err == Aws::S3::S3Errors::NO_SUCH_BUCKET ||
        s3err == Aws::S3::S3Errors::NO_SUCH_KEY ||
        s3err == Aws::S3::S3Errors::RESOURCE_NOT_FOUND ||
        errmsg.find("Response code: 404") != std::string::npos) {
      Log(InfoLogLevel::ERROR_LEVEL, env_->info_log_,
          "[s3] S3GetFileInfo error not-existent %s %s", fname_.c_str(),
          errmsg.c_str());
      return Status::NotFound(fname_, errmsg.c_str());
    }
    Log(InfoLogLevel::ERROR_LEVEL, env_->info_log_,
        "[s3] S3GetFileInfo error %s %s", fname_.c_str(), errmsg.c_str());
    return Status::IOError(fname_, errmsg.c_str());
  }
  const Aws::S3::Model::HeadObjectResult& res = outcome.GetResult();

  // extract data payload
  file_size_ = res.GetContentLength();
  last_mod_time_ = res.GetLastModified().Millis();
  Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
      "[s3] S3GetFileInfo %s size %ld ok", fname_.c_str(), file_size_);
  return Status::OK();
};

/******************** Writablefile ******************/

//
// Create bucket in S3 if it does not already exist.
//
Status S3WritableFile::CreateBucketInS3(
    std::shared_ptr<AwsS3ClientWrapper> client, const std::string& bucket_prefix,
    const Aws::S3::Model::BucketLocationConstraint& location) {
  // specify region for the bucket
  Aws::S3::Model::CreateBucketConfiguration conf;
  if (location != Aws::S3::Model::BucketLocationConstraint::NOT_SET) {
    // only set the location constraint if it's not not set
    conf.SetLocationConstraint(location);
  }

  // create bucket
  Aws::String bucket = GetBucket(bucket_prefix);
  Aws::S3::Model::CreateBucketRequest request;
  request.SetBucket(bucket);
  request.SetCreateBucketConfiguration(conf);
  Aws::S3::Model::CreateBucketOutcome outcome = client->CreateBucket(request);
  bool isSuccess = outcome.IsSuccess();
  if (!isSuccess) {
    const Aws::Client::AWSError<Aws::S3::S3Errors>& error = outcome.GetError();
    std::string errmsg(error.GetMessage().c_str());
    Aws::S3::S3Errors s3err = error.GetErrorType();
    if (s3err != Aws::S3::S3Errors::BUCKET_ALREADY_EXISTS &&
        s3err != Aws::S3::S3Errors::BUCKET_ALREADY_OWNED_BY_YOU) {
      return Status::IOError(bucket.c_str(), errmsg.c_str());
    }
  }
  return Status::OK();
}

S3WritableFile::S3WritableFile(AwsEnv* env, const std::string& local_fname,
                               const std::string& bucket_prefix,
                               const std::string& cloud_fname,
                               const EnvOptions& options,
                               const CloudEnvOptions cloud_env_options)
    : env_(env),
      fname_(local_fname),
      manifest_durable_periodicity_millis_(
          cloud_env_options.manifest_durable_periodicity_millis),
      manifest_last_sync_time_(0) {
  assert(IsSstFile(fname_) || IsManifestFile(fname_));

  // Is this a manifest file?
  is_manifest_ = IsManifestFile(fname_);

  Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
      "[s3] S3WritableFile bucket %s opened local file %s "
      "cloud file %s manifest %d",
      bucket_prefix.c_str(), fname_.c_str(), cloud_fname.c_str(), is_manifest_);

  // Create a temporary file using the posixEnv. This file will be deleted
  // when the file is closed.
  Status s = env_->GetPosixEnv()->NewWritableFile(fname_, &temp_file_, options);
  if (!s.ok()) {
    Log(InfoLogLevel::ERROR_LEVEL, env_->info_log_,
        "[s3] NewWritableFile src %s %s", fname_.c_str(), s.ToString().c_str());
    status_ = s;
  }
  s3_bucket_ = GetBucket(bucket_prefix);
  s3_object_ = Aws::String(cloud_fname.c_str(), cloud_fname.size());
}

S3WritableFile::~S3WritableFile() {
    if (temp_file_ != nullptr) {
      Close();
    }
}

Status S3WritableFile::Close() {
  if (temp_file_ == nullptr) { // already closed
    return status_;
  }
  Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
      "[s3] S3WritableFile closing %s", fname_.c_str());
  assert(status_.ok());

  // close local file
  Status st = temp_file_->Close();
  if (!st.ok()) {
    Log(InfoLogLevel::ERROR_LEVEL, env_->info_log_,
        "[s3] S3WritableFile closing error on local %s\n", fname_.c_str());
    return st;
  }
  temp_file_.reset(nullptr);

  // find file size of local file to be uploaded.
  uint64_t file_size;
  status_ = env_->GetPosixEnv()->GetFileSize(fname_, &file_size);
  if (!status_.ok()) {
    Log(InfoLogLevel::ERROR_LEVEL, env_->info_log_,
        "[s3] S3WritableFile closing error in getting filesize %s %s",
        fname_.c_str(), status_.ToString().c_str());
    return status_;
  }

  // If this is a manifest file, then upload to S3
  // to make it durable. Do not delete local instance of MANIFEST.
  if (is_manifest_) {
    status_ = CopyManifestToS3(file_size, true);
    return status_;
  }

  // upload sst file to S3
  assert(IsSstFile(fname_));
  status_ = CopyToS3(env_, fname_, s3_bucket_, s3_object_, file_size);
  if (!status_.ok()) {
    Log(InfoLogLevel::ERROR_LEVEL, env_->info_log_,
        "[s3] S3WritableFile closing CopyToS3 failed on local file %s",
        fname_.c_str());
    return status_;
  }

  // delete local file
  if (!env_->cloud_env_options.keep_local_sst_files) {
    status_ = env_->GetPosixEnv()->DeleteFile(fname_);
    if (!status_.ok()) {
      Log(InfoLogLevel::ERROR_LEVEL, env_->info_log_,
          "[s3] S3WritableFile closing delete failed on local file %s",
          fname_.c_str());
      return status_;
    }
  }
  Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
      "[s3] S3WritableFile closed file %s size %ld", fname_.c_str(), file_size);
  return Status::OK();
}

// Sync a file to stable storage
Status S3WritableFile::Sync() {
  if (temp_file_ == nullptr) {
    return status_;
  }
  assert(status_.ok());

  // sync local file
  Status stat = temp_file_->Sync();

  // If we are synching a manifest file, then we can copy it to
  // S3 to make it durable
  if (is_manifest_ && stat.ok()) {
    stat = CopyManifestToS3(temp_file_->GetFileSize());
  }
  return stat;
}

//
// Sync this file to the specified S3 object
//
Status S3WritableFile::CopyToS3(const AwsEnv* env, const std::string& fname,
                                const Aws::String& s3_bucket,
                                const Aws::String& s3_object,
                                uint64_t size_hint) {
  {
    // debugging paranoia. Files uploaded to S3 can never be zero size.
    size_t fsize = 0;
    Status statx = env->GetPosixEnv()->GetFileSize(fname, &fsize);
    if (fsize == 0) {
      Log(InfoLogLevel::ERROR_LEVEL, env->info_log_,
          "[s3] CopyToS3 "
          "localpath %s error zero size %s",
          fname.c_str(), statx.ToString().c_str());
      return Status::IOError(fname + " Zero size.");
    }
  }

  auto input_data = Aws::MakeShared<Aws::FStream>(
      s3_object.c_str(), fname.c_str(), std::ios_base::in | std::ios_base::out);

  // Copy entire MANIFEST/IDENTITY/SST file into S3.
  // Writes to an S3 object are atomic.
  Aws::S3::Model::PutObjectRequest put_request;
  put_request.SetBucket(s3_bucket);
  put_request.SetKey(s3_object);
  put_request.SetBody(input_data);

  Aws::S3::Model::PutObjectOutcome put_outcome =
      env->s3client_->PutObject(put_request, size_hint);
  bool isSuccess = put_outcome.IsSuccess();
  if (!isSuccess) {
    const Aws::Client::AWSError<Aws::S3::S3Errors>& error =
        put_outcome.GetError();
    std::string errmsg(error.GetMessage().c_str(), error.GetMessage().size());
    return Status::IOError(fname, errmsg);
  }
  return Status::OK();
}

//
// Copy S3 object to specified file
//
Status S3WritableFile::CopyFromS3(AwsEnv* env,
                                  const Aws::String& s3_bucket,
                                  const std::string& source_object,
                                  const std::string& destination_pathname) {

  Status s;
  Env* localenv = env->GetBaseEnv();
  std::string tmp_destination = destination_pathname + ".tmp";
  Aws::String key(source_object.data(), source_object.size());

  Aws::S3::Model::GetObjectRequest getObjectRequest;
  getObjectRequest.SetBucket(s3_bucket);
  getObjectRequest.SetKey(key);
  getObjectRequest.SetResponseStreamFactory([tmp_destination](){
    return Aws::New<Aws::FStream>(Aws::Utils::ARRAY_ALLOCATION_TAG,
                                  tmp_destination, std::ios_base::out); });
  auto get_outcome = env->s3client_->GetObject(getObjectRequest);

  bool isSuccess = get_outcome.IsSuccess();
  if (!isSuccess) {
    const Aws::Client::AWSError<Aws::S3::S3Errors>& error =
        get_outcome.GetError();
    std::string errmsg(error.GetMessage().c_str(), error.GetMessage().size());
    Log(InfoLogLevel::ERROR_LEVEL, env->info_log_,
        "[s3] CopyFromS3 "
        "bucket %s bucketpath %s error %s.",
        s3_bucket.c_str(), key.c_str(), errmsg.c_str());
    return Status::IOError(errmsg);
  }

  // Paranoia. Files can never be zero size.
  uint64_t file_size;
  s = localenv->GetFileSize(tmp_destination, &file_size);
  if (file_size == 0) {
    s = Status::IOError(tmp_destination +  "Zero size.");
    Log(InfoLogLevel::ERROR_LEVEL, env->info_log_,
        "[s3] CopyFromS3 "
        "bucket %s bucketpath %s size %ld. %s",
        s3_bucket.c_str(), key.c_str(), file_size, s.ToString().c_str());
  }

  if (s.ok()) {
    s = localenv->RenameFile(tmp_destination, destination_pathname);
  }
  Log(InfoLogLevel::INFO_LEVEL, env->info_log_,
      "[s3] CopyFromS3 "
      "bucket %s bucketpath %s size %ld. %s",
      s3_bucket.c_str(), key.c_str(), file_size, s.ToString().c_str());
  return s;
}

//
// Copy this file to a object named MANIFEST in S3
//
Status S3WritableFile::CopyManifestToS3(uint64_t size_hint, bool force) {
  Status stat;

  uint64_t now = env_->NowMicros();
  if (is_manifest_ && (force ||
      (manifest_last_sync_time_ + 1000 * manifest_durable_periodicity_millis_ < now))) {
    // Upload manifest file only if it has not been uploaded in the last
    // manifest_durable_periodicity_millis_  milliseconds.
    stat = CopyToS3(env_, fname_, s3_bucket_, s3_object_, size_hint);

    if (stat.ok()) {
      manifest_last_sync_time_ = now;
      Log(InfoLogLevel::DEBUG_LEVEL, env_->info_log_,
          "[s3] S3WritableFile made manifest %s durable to "
          "bucket %s bucketpath %s.",
          fname_.c_str(), s3_bucket_.c_str(), s3_object_.c_str());

      // If cloud stats are present, record the manifest write and its latency in millis.
      auto stats = env_->cloud_env_options.cloud_statistics;
      if (stats) {
          stats->recordTick(NUMBER_MANIFEST_WRITES, 1);
          stats->measureTime(MANIFEST_WRITES_TIME, env_->s3client_result_.micros / 1000);
      }
    } else {
      Log(InfoLogLevel::ERROR_LEVEL, env_->info_log_,
          "[s3] S3WritableFile failed to make manifest %s durable to "
          "bucket %s bucketpath. %s",
          fname_.c_str(), s3_bucket_.c_str(), s3_object_.c_str(),
          stat.ToString().c_str());
    }
  }

  return stat;
}

}  // namespace

#endif /* USE_AWS */
