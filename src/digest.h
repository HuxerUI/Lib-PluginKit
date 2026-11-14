#pragma once

#include "platform_files.h"

#if defined(_WIN32)
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif

#include <algorithm>
#include <array>
#include <memory>

namespace huxerui::pluginkit::detail {

// Supplies bounded chunks to the platform hasher when the input is a file.
template<class Feed>
std::string ComputeSha256(Feed feed) {
  std::array<unsigned char, 32> digest{};
#if defined(_WIN32)
  BCRYPT_ALG_HANDLE raw_algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&raw_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
    throw Error(ErrorCode::Io, "Cannot open SHA-256 provider");
  }
  const auto close_provider = [](void* value) { BCryptCloseAlgorithmProvider(value, 0); };
  const std::unique_ptr<void, decltype(close_provider)> algorithm(raw_algorithm, close_provider);
  BCRYPT_HASH_HANDLE raw_hash = nullptr;
  if (BCryptCreateHash(algorithm.get(), &raw_hash, nullptr, 0, nullptr, 0, 0) < 0) {
    throw Error(ErrorCode::Io, "Cannot create SHA-256 hash");
  }
  const std::unique_ptr<void, decltype(&BCryptDestroyHash)> hash(raw_hash, BCryptDestroyHash);
  feed([&](std::span<const std::byte> bytes) {
    auto* input = reinterpret_cast<unsigned char*>(const_cast<std::byte*>(bytes.data()));
    if (!bytes.empty() && BCryptHashData(hash.get(), input, static_cast<ULONG>(bytes.size()), 0) < 0) {
      throw Error(ErrorCode::Io, "Cannot update SHA-256 hash");
    }
  });
  if (BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
    throw Error(ErrorCode::Io, "Cannot compute SHA-256 digest");
  }
#elif defined(__APPLE__)
  CC_SHA256_CTX context;
  if (CC_SHA256_Init(&context) != 1) throw Error(ErrorCode::Io, "Cannot create SHA-256 hash");
  feed([&](std::span<const std::byte> bytes) {
    if (!bytes.empty() && CC_SHA256_Update(&context, bytes.data(), static_cast<CC_LONG>(bytes.size())) != 1) {
      throw Error(ErrorCode::Io, "Cannot update SHA-256 hash");
    }
  });
  if (CC_SHA256_Final(digest.data(), &context) != 1) {
    throw Error(ErrorCode::Io, "Cannot compute SHA-256 digest");
  }
#else
  const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
    throw Error(ErrorCode::Io, "Cannot create SHA-256 hash");
  }
  feed([&](std::span<const std::byte> bytes) {
    if (!bytes.empty() && EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1) {
      throw Error(ErrorCode::Io, "Cannot update SHA-256 hash");
    }
  });
  unsigned size = 0;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &size) != 1 || size != digest.size()) {
    throw Error(ErrorCode::Io, "Cannot compute SHA-256 digest");
  }
#endif

  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    result.push_back(digits[byte >> 4]);
    result.push_back(digits[byte & 15]);
  }
  return result;
}

inline std::string Sha256(std::span<const std::byte> bytes) {
  return ComputeSha256([&](const auto& update) {
    constexpr std::size_t MaxChunk = 64 * 1024;
    while (!bytes.empty()) {
      const auto size = std::min(bytes.size(), MaxChunk);
      update(bytes.first(size));
      bytes = bytes.subspan(size);
    }
  });
}

inline std::string Sha256File(const std::filesystem::path& path) {
  const auto file = OpenRegularFile(path);
  return ComputeSha256([&](const auto& update) { ReadFileChunks(file, update); });
}

inline bool IsDigest(std::string_view text) {
  if (text.size() != 64) return false;
  for (const auto character : text) {
    if ((character < '0' || character > '9') && (character < 'a' || character > 'f')) return false;
  }
  return true;
}

inline bool IsId(std::string_view text) {
  if (text.empty() || text.size() > 128) return false;
  const auto alphanumeric = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
  if (!alphanumeric(text.front())) return false;
  for (const auto character : text) {
    if (!alphanumeric(character) && character != '.' && character != '_' && character != '-') return false;
  }
  return true;
}

} // namespace huxerui::pluginkit::detail
