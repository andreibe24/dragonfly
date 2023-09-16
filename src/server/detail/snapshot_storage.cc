// Copyright 2023, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.

#include "server/detail/snapshot_storage.h"

#include <absl/strings/str_replace.h>
#include <absl/strings/strip.h>

#include <regex>

#include "base/logging.h"
#include "io/file_util.h"
#include "server/engine_shard_set.h"
#include "util/cloud/s3.h"
#include "util/fibers/fiber_file.h"
#include "util/uring/uring_file.h"

namespace dfly {
namespace detail {

namespace {

const std::string kTimestampRegex = "([0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2})";

}  // namespace

std::optional<std::pair<std::string, std::string>> GetBucketPath(std::string_view path) {
  std::string_view clean = absl::StripPrefix(path, kS3Prefix);

  size_t pos = clean.find('/');
  if (pos == std::string_view::npos)
    return std::nullopt;

  std::string bucket_name{clean.substr(0, pos)};
  std::string obj_path{clean.substr(pos + 1)};
  return std::make_pair(std::move(bucket_name), std::move(obj_path));
}

#ifdef __linux__
const int kRdbWriteFlags = O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC | O_DIRECT;
#endif

FileSnapshotStorage::FileSnapshotStorage(FiberQueueThreadPool* fq_threadpool)
    : fq_threadpool_{fq_threadpool} {
}

io::Result<std::pair<io::Sink*, uint8_t>, GenericError> FileSnapshotStorage::OpenWriteFile(
    const std::string& path) {
  if (fq_threadpool_) {  // EPOLL
    auto res = util::OpenFiberWriteFile(path, fq_threadpool_);
    if (!res) {
      return nonstd::make_unexpected(GenericError(res.error(), "Couldn't open file for writing"));
    }

    return std::pair(*res, FileType::FILE);
  } else {
#ifdef __linux__
    auto res = util::fb2::OpenLinux(path, kRdbWriteFlags, 0666);
    if (!res) {
      return nonstd::make_unexpected(GenericError(
          res.error(),
          "Couldn't open file for writing (is direct I/O supported by the file system?)"));
    }

    uint8_t file_type = FileType::FILE | FileType::IO_URING;
    if (kRdbWriteFlags & O_DIRECT) {
      file_type |= FileType::DIRECT;
    }
    return std::pair(new LinuxWriteWrapper(res->release()), file_type);
#else
    LOG(FATAL) << "Linux I/O is not supported on this platform";
#endif
  }
}

io::ReadonlyFileOrError FileSnapshotStorage::OpenReadFile(const std::string& path) {
#ifdef __linux__
  if (fq_threadpool_) {
    return util::OpenFiberReadFile(path, fq_threadpool_);
  } else {
    return util::fb2::OpenRead(path);
  }
#else
  return util::OpenFiberReadFile(path, fq_threadpool_);
#endif
}

io::Result<std::string, GenericError> FileSnapshotStorage::LoadPath(std::string_view dir,
                                                                    std::string_view dbfilename) {
  if (dbfilename.empty())
    return "";

  fs::path data_folder;
  if (dir.empty()) {
    data_folder = fs::current_path();
  } else {
    std::error_code file_ec;
    data_folder = fs::canonical(dir, file_ec);
    if (file_ec) {
      return nonstd::make_unexpected(GenericError{file_ec, "Data directory error"});
    }
  }

  LOG(INFO) << "Load snapshot: Searching for snapshot in directory: " << data_folder;

  fs::path fl_path = data_folder.append(dbfilename);
  // If we've found an exact match we're done.
  if (fs::exists(fl_path))
    return fl_path.generic_string();

  SubstituteFilenameTsPlaceholder(&fl_path, "*");
  if (!fl_path.has_extension()) {
    fl_path += "*";
  }
  io::Result<io::StatShortVec> short_vec = io::StatFiles(fl_path.generic_string());
  if (short_vec) {
    // io::StatFiles returns a list of sorted files. Because our timestamp format has the same
    // time order and lexicographic order we iterate from the end to find the latest snapshot.
    auto it = std::find_if(short_vec->rbegin(), short_vec->rend(), [](const auto& stat) {
      return absl::EndsWith(stat.name, ".rdb") || absl::EndsWith(stat.name, "summary.dfs");
    });
    if (it != short_vec->rend())
      return it->name;
  } else {
    return nonstd::make_unexpected(
        GenericError(short_vec.error(), "Could not stat snapshot directory"));
  }

  return nonstd::make_unexpected(GenericError(
      std::make_error_code(std::errc::no_such_file_or_directory), "Snapshot not found"));
}

io::Result<std::vector<std::string>, GenericError> FileSnapshotStorage::LoadPaths(
    const std::string& load_path) {
  if (!(absl::EndsWith(load_path, ".rdb") || absl::EndsWith(load_path, "summary.dfs"))) {
    return nonstd::make_unexpected(
        GenericError(std::make_error_code(std::errc::invalid_argument), "Bad filename extension"));
  }

  std::vector<std::string> paths{{load_path}};

  // Collect all other files in case we're loading dfs.
  if (absl::EndsWith(load_path, "summary.dfs")) {
    std::string glob = absl::StrReplaceAll(load_path, {{"summary", "????"}});
    io::Result<io::StatShortVec> files = io::StatFiles(glob);

    if (files && files->size() == 0) {
      return nonstd::make_unexpected(
          GenericError(std::make_error_code(std::errc::no_such_file_or_directory),
                       "Cound not find DFS shard files"));
    }

    for (auto& fstat : *files) {
      paths.push_back(std::move(fstat.name));
    }
  }

  // Check all paths are valid.
  for (const auto& path : paths) {
    std::error_code ec;
    (void)fs::canonical(path, ec);
    if (ec) {
      return nonstd::make_unexpected(ec);
    }
  }

  return paths;
}

AwsS3SnapshotStorage::AwsS3SnapshotStorage(util::cloud::AWS* aws) : aws_{aws} {
}

io::Result<std::pair<io::Sink*, uint8_t>, GenericError> AwsS3SnapshotStorage::OpenWriteFile(
    const std::string& path) {
  DCHECK(aws_);

  std::optional<std::pair<std::string, std::string>> bucket_path = GetBucketPath(path);
  if (!bucket_path) {
    return nonstd::make_unexpected(GenericError("Invalid S3 path"));
  }
  auto [bucket_name, obj_path] = *bucket_path;

  util::cloud::S3Bucket bucket(*aws_, bucket_name);
  std::error_code ec = bucket.Connect(kBucketConnectMs);
  if (ec) {
    return nonstd::make_unexpected(GenericError(ec, "Couldn't connect to S3 bucket"));
  }
  auto res = bucket.OpenWriteFile(obj_path);
  if (!res) {
    return nonstd::make_unexpected(GenericError(res.error(), "Couldn't open file for writing"));
  }

  return std::pair<io::Sink*, uint8_t>(*res, FileType::CLOUD);
}

io::ReadonlyFileOrError AwsS3SnapshotStorage::OpenReadFile(const std::string& path) {
  std::optional<std::pair<std::string, std::string>> bucket_path = GetBucketPath(path);
  if (!bucket_path) {
    return nonstd::make_unexpected(GenericError("Invalid S3 path"));
  }
  auto [bucket_name, obj_path] = *bucket_path;

  util::cloud::S3Bucket bucket{*aws_, bucket_name};
  std::error_code ec = bucket.Connect(kBucketConnectMs);
  if (ec) {
    return nonstd::make_unexpected(GenericError(ec, "Couldn't connect to S3 bucket"));
  }
  auto res = bucket.OpenReadFile(obj_path);
  if (!res) {
    return nonstd::make_unexpected(GenericError(res.error(), "Couldn't open file for reading"));
  }
  return res;
}

io::Result<std::string, GenericError> AwsS3SnapshotStorage::LoadPath(std::string_view dir,
                                                                     std::string_view dbfilename) {
  if (dbfilename.empty())
    return "";

  std::optional<std::pair<std::string, std::string>> bucket_path = GetBucketPath(dir);
  if (!bucket_path) {
    return nonstd::make_unexpected(
        GenericError{std::make_error_code(std::errc::invalid_argument), "Invalid S3 path"});
  }
  auto [bucket_name, prefix] = *bucket_path;

  util::fb2::ProactorBase* proactor = shard_set->pool()->GetNextProactor();
  return proactor->Await([&]() -> io::Result<std::string, GenericError> {
    LOG(INFO) << "Load snapshot: Searching for snapshot in S3 path: " << kS3Prefix << bucket_name
              << "/" << prefix;

    // Create a regex to match the object keys, substituting the timestamp
    // and adding an extension if needed.
    fs::path fl_path{prefix};
    fl_path.append(dbfilename);
    SubstituteFilenameTsPlaceholder(&fl_path, kTimestampRegex);
    if (!fl_path.has_extension()) {
      fl_path += "(-summary.dfs|.rdb)";
    }
    const std::regex re(fl_path.string());

    // Sort the keys in reverse so the first. Since the timestamp format
    // has lexicographic order, the matching snapshot file will be the latest
    // snapshot.
    io::Result<std::vector<std::string>, GenericError> keys = ListObjects(bucket_name, prefix);
    if (!keys) {
      return nonstd::make_unexpected(keys.error());
    }

    std::sort(std::rbegin(*keys), std::rend(*keys));
    for (const std::string& key : *keys) {
      std::smatch m;
      if (std::regex_match(key, m, re)) {
        return std::string(kS3Prefix) + bucket_name + "/" + key;
      }
    }

    return nonstd::make_unexpected(GenericError(
        std::make_error_code(std::errc::no_such_file_or_directory), "Snapshot not found"));
  });
}

io::Result<std::vector<std::string>, GenericError> AwsS3SnapshotStorage::LoadPaths(
    const std::string& load_path) {
  if (!(absl::EndsWith(load_path, ".rdb") || absl::EndsWith(load_path, "summary.dfs"))) {
    return nonstd::make_unexpected(
        GenericError(std::make_error_code(std::errc::invalid_argument), "Bad filename extension"));
  }

  // Find snapshot shard files if we're loading DFS.
  if (absl::EndsWith(load_path, "summary.dfs")) {
    util::fb2::ProactorBase* proactor = shard_set->pool()->GetNextProactor();
    return proactor->Await([&]() -> io::Result<std::vector<std::string>, GenericError> {
      std::vector<std::string> paths{{load_path}};

      std::optional<std::pair<std::string, std::string>> bucket_path = GetBucketPath(load_path);
      if (!bucket_path) {
        return nonstd::make_unexpected(
            GenericError{std::make_error_code(std::errc::invalid_argument), "Invalid S3 path"});
      }
      const auto [bucket_name, obj_path] = *bucket_path;

      const std::regex re(absl::StrReplaceAll(obj_path, {{"summary", "[0-9]{4}"}}));

      // Limit prefix to objects in the same 'directory' as load_path.
      const size_t pos = obj_path.find_last_of('/');
      const std::string prefix = (pos == std::string_view::npos) ? "" : obj_path.substr(0, pos);
      const io::Result<std::vector<std::string>, GenericError> keys =
          ListObjects(bucket_name, prefix);
      if (!keys) {
        return nonstd::make_unexpected(keys.error());
      }

      for (const std::string& key : *keys) {
        std::smatch m;
        if (std::regex_match(key, m, re)) {
          paths.push_back(std::string(kS3Prefix) + bucket_name + "/" + key);
        }
      }

      if (paths.size() <= 1) {
        return nonstd::make_unexpected(
            GenericError{std::make_error_code(std::errc::no_such_file_or_directory),
                         "Cound not find DFS snapshot shard files"});
      }

      return paths;
    });
  }

  return std::vector<std::string>{{load_path}};
}

io::Result<std::vector<std::string>, GenericError> AwsS3SnapshotStorage::ListObjects(
    std::string_view bucket_name, std::string_view prefix) {
  util::cloud::S3Bucket bucket(*aws_, bucket_name);
  std::error_code ec = bucket.Connect(kBucketConnectMs);
  if (ec) {
    return nonstd::make_unexpected(GenericError{ec, "Couldn't connect to S3 bucket"});
  }

  std::vector<std::string> keys;
  ec = bucket.ListAllObjects(
      prefix, [&](size_t sz, std::string_view name) { keys.push_back(std::string(name)); });
  if (ec) {
    return nonstd::make_unexpected(GenericError{ec, "Couldn't list objects in S3 bucket"});
  }

  return keys;
}

io::Result<size_t> LinuxWriteWrapper::WriteSome(const iovec* v, uint32_t len) {
  io::Result<size_t> res = lf_->WriteSome(v, len, offset_, 0);
  if (res) {
    offset_ += *res;
  }

  return res;
}

void SubstituteFilenameTsPlaceholder(fs::path* filename, std::string_view replacement) {
  *filename = absl::StrReplaceAll(filename->string(), {{"{timestamp}", replacement}});
}

}  // namespace detail
}  // namespace dfly