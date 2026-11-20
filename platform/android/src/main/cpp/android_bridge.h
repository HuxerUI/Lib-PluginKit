#pragma once

#include <huxerui/pluginkit/contract.h>
#include <jni.h>
#include <chrono>
#include <span>

namespace huxerui::pluginkit::detail {

/// Attaches transport threads only for the duration of their Java platform operation.
class AndroidEnvironment {
public:
  AndroidEnvironment();
  AndroidEnvironment(const AndroidEnvironment&) = delete;
  AndroidEnvironment& operator=(const AndroidEnvironment&) = delete;
  ~AndroidEnvironment();
  JNIEnv* operator->() const { return environment_; }
  operator JNIEnv*() const { return environment_; }
private:
  JNIEnv* environment_ = nullptr;
  bool attached_ = false;
};

void CheckAndroidException(JNIEnv* environment, ErrorCode code = ErrorCode::Io);
struct AndroidSession { int socket; std::uint32_t pid; std::int64_t id; };
AndroidSession OpenAndroidSession(const std::string& path, int module, std::chrono::milliseconds timeout);
void CloseAndroidSession(std::int64_t id, bool graceful) noexcept;

/// Streams SHA-256 through Android's public MessageDigest API, without private system libraries.
class AndroidDigest {
public:
  AndroidDigest();
  AndroidDigest(const AndroidDigest&) = delete;
  AndroidDigest& operator=(const AndroidDigest&) = delete;
  ~AndroidDigest();
  void Update(std::span<const std::byte> bytes);
  void Finish(std::span<unsigned char, 32> output);
private:
  AndroidEnvironment environment_;
  jobject digest_ = nullptr;
  jmethodID update_ = nullptr;
};

} // namespace huxerui::pluginkit::detail
