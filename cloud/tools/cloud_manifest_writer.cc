// Copyright (c) 2017-present, Rockset, Inc.  All rights reserved.

#if !(defined GFLAGS) || !(defined USE_AWS) || defined(ROCKSDB_LITE)

#include <cstdio>
int main() {
#ifndef GFLAGS
  fprintf(stderr, "Please install gflags to run rocksdb tools\n");
#endif
#ifndef USE_AWS
  fprintf(stderr, "Please compile with USE_AWS=1\n");
#endif
#ifdef ROCKSDB_LITE
  fprintf(stderr, "DbDumpTool is not supported in ROCKSDB_LITE\n");
#endif
  return 1;
}

#else

//#include "rocksdb/convenience.h"
//#include "rocksdb/db_dump_tool.h"

#include <cstdio>
#include <iostream>
#include <algorithm>

#include "cloud/cloud_env_impl.h"
#include "cloud/cloud_manifest.h"
#include "cloud/filename.h"
#include "cloud/aws/aws_env.h"
#include "rocksdb/cloud/cloud_env_options.h"
#include "util/file_reader_writer.h"
#include "util/gflags_compat.h"

DEFINE_string(dbpath, "/tmp/tsdb", "Path to the local db");
DEFINE_string(cloud_src_bucket, "N/A", "Source bucket suffix in the cloud.");
DEFINE_string(cloud_src_dbpath, "N/A", "Source path to db relative to bucket.");
DEFINE_string(cloud_dest_bucket, "N/A", "Destination bucket suffix in the cloud.");
DEFINE_string(cloud_dest_dbpath, "N/A", "Destination path to db relative to bucket.");
DEFINE_string(aws_region, "us-west-2", "AWS region");
DEFINE_string(aws_access_key_id, "N/A", "AWS Access Key Id.");
DEFINE_string(aws_secret_access_key, "N/A", "AWS Secret Access Key");

using namespace rocksdb;

class Migration {

  public:
    Migration(
      std::string & local_dbpath,
      std::string & cloud_src_bucket,
      std::string & cloud_src_dbpath,
      std::string & cloud_dest_bucket,
      std::string & cloud_dest_dbpath,
      std::string & aws_region,
      std::string & aws_access_key_id,
      std::string & aws_secret_access_key
    ) : local_dbpath_(local_dbpath) {

      // cloud environment config options here
      CloudEnvOptions cloud_env_options;
      cloud_env_options.credentials.access_key_id.assign(aws_access_key_id);
      cloud_env_options.credentials.secret_key.assign(aws_secret_access_key);

      // Create a new AWS cloud env Status
      CloudEnv* cenv;
      Status sAws = CloudEnv::NewAwsEnv(Env::Default(),
                                        cloud_src_bucket,
                                        cloud_src_dbpath,
                                        aws_region,
                                        cloud_dest_bucket,
                                        cloud_dest_dbpath,
                                        aws_region,
                                        cloud_env_options,
                                        nullptr,
                                        &cenv);

      if (!sAws.ok()) {
        std::cout << "Unable to create CloudEnv with src["
          << cloud_src_bucket << cloud_src_dbpath << "], dest["
          << cloud_dest_bucket << cloud_dest_dbpath << "], in region "
          << aws_region << ". Error: " << sAws.ToString();
      }
      cloud_env_.reset(cenv);
    }
  
    ~Migration() {}

    Status createNewCloudManifest() {
      Env* env_ = Env::Default();

      std::unique_ptr<CloudManifest> manifest;
      Status s = CloudManifest::CreateForEmptyDatabase("", &manifest);
      if (!s.ok()) {
        std::cout << "Could not create cloud manifest: " << s.ToString() << std::endl;
        return s;
      }

      auto filename = CloudManifestFile(local_dbpath_);
      {
        std::unique_ptr<WritableFile> file;
        s = env_->NewWritableFile(filename, &file, EnvOptions());
        if (!s.ok()) {
          std::cout << "Could not write " << filename << ": " << s.ToString() << std::endl;
          return s;
        }
        s = manifest->WriteToLog(std::unique_ptr<WritableFileWriter>(
          new WritableFileWriter(std::move(file), EnvOptions())));
        if (!s.ok()) {
          std::cout << "Could not write to log: " << s.ToString() << std::endl;
          return s;
        }
      }

      manifest.reset();
      {
        std::unique_ptr<SequentialFile> file;
        s = env_->NewSequentialFile(filename, &file, EnvOptions());
        if (!s.ok()) {
          std::cout << "Could not initialize reader for "
            << filename << ": " << s.ToString() << std::endl;
          return s;
        }
        CloudManifest::LoadFromLog(
          std::unique_ptr<SequentialFileReader>(
            new SequentialFileReader(std::move(file))),
            &manifest);
        if (!s.ok()) {
          std::cout << "Could not read back the "
            << filename << ": " << s.ToString() << std::endl;
          return s;
        }
      }
      return s;
    }

    Status uploadCloudManifest() {
      CloudEnv* cenv = cloud_env_.get();
      if (cenv->GetDestBucketPrefix().empty()) {
        std::cout << "destination bucket is not configured." << std::endl;
        return Status::InvalidArgument(Status::kNone);
      }

      // make sure to not overwrite the CLOUDMANIFEST in destination bucket
      Status st = cenv->ExistsObject(cenv->GetDestBucketPrefix(),
                               CloudManifestFile(cenv->GetDestObjectPrefix()));
      if (!st.IsNotFound()) {
        // already exists in destination bucket
        std::cout << "CLOUDMANIFEST already exists in the destination bucket." << std::endl;
        return Status::Aborted(Status::kNone);
      }

      // it is safe to upload
      return cenv->PutObject(CloudManifestFile(local_dbpath_),
                             cenv->GetDestBucketPrefix(),
                             CloudManifestFile(cenv->GetDestObjectPrefix()));
    }

    Status createNewCloudIdentity() {
      return CreateNewIdentityFile(cloud_env_->GenerateUniqueId());
    }

  private:
    const std::string local_dbpath_;
    std::unique_ptr<CloudEnv> cloud_env_;

    Status CreateNewIdentityFile(const std::string& new_dbid) {
      const Options options;
      const EnvOptions soptions;
      auto tmp_identity_path = local_dbpath_ + "/IDENTITY.tmp";
      CloudEnv* cenv = cloud_env_.get();
      Env* env = cenv->GetBaseEnv();
      Status st;
      {
        unique_ptr<WritableFile> destfile;
        st = env->NewWritableFile(tmp_identity_path, &destfile, soptions);
        if (!st.ok()) {
          std::cout << "Unable to create local IDENTITY file to "
            << tmp_identity_path << ". Error: " << st.ToString() << std::endl;
          return st;
        }
        st = destfile->Append(Slice(new_dbid));
        if (!st.ok()) {
          std::cout << "Unable to write new dbid to local IDENTITY file "
            << tmp_identity_path << ". Error: " << st.ToString() << std::endl;
          return st;
        }
      }

      // Rename ID file on local filesystem and upload it to dest bucket too
      st = cenv->RenameFile(tmp_identity_path, local_dbpath_ + "/IDENTITY");
      if (!st.ok()) {
        std::cout << "Unable to rename newly created IDENTITY.tmp"
            << " to IDENTITY. Error: " << st.ToString() << std::endl;
        return st;
      }

      // Read dbid into string, because that is not the same as the content in new_dbid content
      std::string dbid;
      st = ReadFileToString(env, local_dbpath_ + "/IDENTITY", &dbid);
      dbid = trim(dbid);
      std::cout << "Written new dbid " << dbid << " " << st.ToString() << std::endl;

      // Save the dbid to destination data path mapping into AWS
      AwsEnv* awsEnv = static_cast<AwsEnv*>(cenv);
      std::string dest_data_path = awsEnv->GetDestObjectPrefix();
      std::cout << "SaveDbid " << dbid << " for " << dest_data_path << std::endl;
      return awsEnv->SaveDbid(dbid, dest_data_path);
    }
};

// creates a CLOUDMANIFEST file.
// This file can be used to migrate the old database format to cloud.
int main(int argc, char** argv) {
  GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);

  Migration m(FLAGS_dbpath,
              FLAGS_cloud_src_bucket,
              FLAGS_cloud_src_dbpath,
              FLAGS_cloud_dest_bucket,
              FLAGS_cloud_dest_dbpath,
              FLAGS_aws_region,
              FLAGS_aws_access_key_id,
              FLAGS_aws_secret_access_key);
  Status s = m.createNewCloudManifest();
  if (!s.ok()) {
    std::cout << "createNewCloudManifest failed with " << s.ToString() << std::endl;
    return -1;
  }
  s = m.uploadCloudManifest();
  if (!s.ok()) {
    std::cout << "uploadCloudManifest failed with " << s.ToString() << std::endl;
    return -1;
  }
  s = m.createNewCloudIdentity();
  if (!s.ok()) {
    std::cout << "createNewCloudIdentity failed with " << s.ToString() << std::endl;
    return -1;
  }
  return 0;
}
#endif  // !(defined GFLAGS) || defined(ROCKSDB_LITE)