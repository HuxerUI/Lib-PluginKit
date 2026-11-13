#pragma once

#include "platform_files.h"

#if defined(_WIN32)
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif

#include <array>

namespace huxerui::pluginkit::detail {

inline std::string Sha256(std::span<const std::byte> bytes) {
  std::array<unsigned char, 32> digest{};
#if defined(_WIN32)
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
    throw Error(ErrorCode::Io, "Cannot open SHA-256 provider");
  }
  auto* input = reinterpret_cast<unsigned char*>(const_cast<std::byte*>(bytes.data()));
  const auto status = BCryptHash(algorithm, nullptr, 0, input,
                                 static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size()));
  BCryptCloseAlgorithmProvider(algorithm, 0);
  if (status < 0) throw Error(ErrorCode::Io, "Cannot compute SHA-256 digest");
#elif defined(__APPLE__)
  if (bytes.size() > (std::numeric_limits<CC_LONG>::max)() ||
      !CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest.data())) {
    throw Error(ErrorCode::Io, "Cannot compute SHA-256 digest");
  }
#else
  unsigned size = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1 ||
      size != digest.size()) {
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
