#include "android_bridge.h"
#include "posix_socket.h"
#include "worker_server.h"

#include <huxerui/app.h>
#include <huxerui/android/jni.h>
#include <array>
#include <thread>

namespace huxerui::pluginkit::detail {
namespace {
JavaVM* java_vm = nullptr;
jclass plugin_kit = nullptr;
std::optional<Application> worker_application;
std::optional<TaskScope> worker_tasks;
std::mutex worker_state;

void ThrowJava(JNIEnv* environment, const std::exception& error) {
  const auto type = environment->FindClass("java/lang/IllegalStateException");
  if (type) environment->ThrowNew(type, error.what());
}
} // namespace

AndroidEnvironment::AndroidEnvironment() {
  if (!java_vm) throw Error(ErrorCode::Io, "PluginKit.initializeProcess must run before native plugin operations");
  const auto status = java_vm->GetEnv(reinterpret_cast<void**>(&environment_), JNI_VERSION_1_6);
  if (status == JNI_EDETACHED) {
    if (java_vm->AttachCurrentThread(&environment_, nullptr) != JNI_OK) throw Error(ErrorCode::Io, "Cannot attach Android transport thread");
    attached_ = true;
  } else if (status != JNI_OK) throw Error(ErrorCode::Io, "Cannot obtain Android JNI environment");
}
AndroidEnvironment::~AndroidEnvironment() { if (attached_) java_vm->DetachCurrentThread(); }

void CheckAndroidException(JNIEnv* environment, ErrorCode code) {
  if (!environment->ExceptionCheck()) return;
  auto exception = environment->ExceptionOccurred();
  environment->ExceptionClear();
  const auto timeout = environment->FindClass("java/util/concurrent/TimeoutException");
  if (timeout && environment->IsInstanceOf(exception, timeout)) code = ErrorCode::DeadlineExceeded;
  if (timeout) environment->DeleteLocalRef(timeout);
  const auto type = environment->GetObjectClass(exception);
  const auto method = environment->GetMethodID(type, "toString", "()Ljava/lang/String;");
  auto value = static_cast<jstring>(environment->CallObjectMethod(exception, method));
  std::string message = "Android plugin operation failed";
  if (value && !environment->ExceptionCheck()) {
    message = android::JavaStringToUtf8(environment, value);
  }
  environment->ExceptionClear();
  if (value) environment->DeleteLocalRef(value);
  environment->DeleteLocalRef(type);
  environment->DeleteLocalRef(exception);
  throw Error(code, message);
}

AndroidSession OpenAndroidSession(const std::string& path, int module, std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) throw Error(ErrorCode::DeadlineExceeded, "Worker startup deadline expired");
  AndroidEnvironment environment;
  const auto method = environment->GetStaticMethodID(plugin_kit, "open", "(Ljava/lang/String;IJ)[J");
  auto name = android::Utf8ToJavaString(environment, path);
  auto result = static_cast<jlongArray>(environment->CallStaticObjectMethod(plugin_kit, method, name.Get(), module, static_cast<jlong>(timeout.count())));
  CheckAndroidException(environment, ErrorCode::WorkerExited);
  std::array<jlong, 3> values{};
  if (!result || environment->GetArrayLength(result) != 3) throw Error(ErrorCode::Protocol, "Invalid Android session");
  environment->GetLongArrayRegion(result, 0, 3, values.data());
  environment->DeleteLocalRef(result);
  CheckAndroidException(environment);
  return {static_cast<int>(values[0]), static_cast<std::uint32_t>(values[1]), values[2]};
}

void CloseAndroidSession(std::int64_t id, bool graceful) noexcept {
  try {
    AndroidEnvironment environment;
    const auto method = environment->GetStaticMethodID(plugin_kit, "release", "(JZ)V");
    environment->CallStaticVoidMethod(plugin_kit, method, static_cast<jlong>(id), static_cast<jboolean>(graceful));
    CheckAndroidException(environment);
  } catch (...) {
    // Socket teardown still wakes native waiters if Java is already shutting down.
  }
}

AndroidDigest::AndroidDigest() {
  auto type = environment_->FindClass("java/security/MessageDigest");
  auto factory = environment_->GetStaticMethodID(type, "getInstance", "(Ljava/lang/String;)Ljava/security/MessageDigest;");
  auto algorithm = environment_->NewStringUTF("SHA-256");
  digest_ = environment_->CallStaticObjectMethod(type, factory, algorithm);
  environment_->DeleteLocalRef(algorithm);
  CheckAndroidException(environment_);
  update_ = environment_->GetMethodID(type, "update", "([B)V");
  environment_->DeleteLocalRef(type);
}
AndroidDigest::~AndroidDigest() { if (digest_) environment_->DeleteLocalRef(digest_); }
void AndroidDigest::Update(std::span<const std::byte> bytes) {
  auto array = environment_->NewByteArray(static_cast<jsize>(bytes.size()));
  CheckAndroidException(environment_);
  environment_->SetByteArrayRegion(array, 0, static_cast<jsize>(bytes.size()), reinterpret_cast<const jbyte*>(bytes.data()));
  environment_->CallVoidMethod(digest_, update_, array);
  environment_->DeleteLocalRef(array);
  CheckAndroidException(environment_);
}
void AndroidDigest::Finish(std::span<unsigned char, 32> output) {
  auto type = environment_->GetObjectClass(digest_);
  auto method = environment_->GetMethodID(type, "digest", "()[B");
  auto array = static_cast<jbyteArray>(environment_->CallObjectMethod(digest_, method));
  CheckAndroidException(environment_);
  if (environment_->GetArrayLength(array) != 32) throw Error(ErrorCode::Io, "Invalid SHA-256 digest");
  environment_->GetByteArrayRegion(array, 0, 32, reinterpret_cast<jbyte*>(output.data()));
  environment_->DeleteLocalRef(array);
  environment_->DeleteLocalRef(type);
  CheckAndroidException(environment_);
}

} // namespace huxerui::pluginkit::detail

using namespace huxerui;
using namespace huxerui::pluginkit::detail;

extern "C" JNIEXPORT void JNICALL
Java_org_huxerui_lib_pluginkit_PluginKit_nativeInitialize(JNIEnv* environment, jclass type, jboolean worker) {
  try {
    environment->GetJavaVM(&java_vm);
    if (!plugin_kit) plugin_kit = static_cast<jclass>(environment->NewGlobalRef(type));
    if (worker && !worker_application) worker_application.emplace(+[]() -> View { return {}; });
  } catch (const std::exception& error) { ThrowJava(environment, error); }
}

extern "C" JNIEXPORT void JNICALL
Java_org_huxerui_lib_pluginkit_PluginKit_nativeReady(JNIEnv* environment, jclass) {
  try { std::lock_guard lock(worker_state); worker_tasks.emplace(UseApplicationTaskScope()); }
  catch (const std::exception& error) { ThrowJava(environment, error); }
}

extern "C" JNIEXPORT void JNICALL
Java_org_huxerui_lib_pluginkit_PluginKit_nativeOpen(JNIEnv* environment, jclass, jstring path, jint socket, jint module) {
  Handle transport(socket);
  Handle library(module);
  try {
    auto tasks = [] {
      std::lock_guard lock(worker_state);
      if (!worker_tasks) throw std::logic_error("Android worker Runtime is not ready");
      return *worker_tasks;
    }();
    std::filesystem::path name(android::JavaStringToUtf8(environment, path));
    std::thread([transport = std::move(transport), library = std::move(library), name = std::move(name), tasks] {
      try {
        worker_server::Server server(name, library.get());
        const SocketStream stream(transport.get());
        auto read = [&stream](std::span<std::byte> bytes) { stream.Read(bytes); };
        auto write = [&stream](std::span<const std::byte> bytes) { stream.Write(bytes); };
        worker_server::Exchange(read, write, tasks, server);
      } catch (...) { std::_Exit(1); }
    }).detach();
  } catch (const std::exception& error) { ThrowJava(environment, error); }
}
