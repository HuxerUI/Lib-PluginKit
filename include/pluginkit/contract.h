#pragma once

/// @file
/// @brief Contract metadata shared by the host and independently built plugins.

#include <cstdint>
#include <string_view>

namespace pluginkit {

/// @brief Exact identity of a generated contract.
///
/// String views borrow their underlying storage. Generated bindings use static storage for both strings.
struct ContractInfo {
  /// Stable contract identifier.
  std::string_view id;
  /// Positive version supplied to hpk gen.
  std::uint32_t version;
  /// Lowercase SHA-256 of the generated contract schema.
  std::string_view fingerprint;

  /// @brief Compares contract identifiers, versions, and fingerprints.
  /// @return True when all three values match.
  bool operator==(const ContractInfo&) const = default;
};

/// @brief Specialized by the SDK generator with metadata and a dispatcher.
/// @tparam Interface C++ interface passed to hpk gen.
template<class Interface>
struct Contract;

/// @brief Associates a typed contract with an extension point registered by the host.
/// @tparam Interface Interface accepted by the point's contributions.
template<class Interface>
struct ExtensionPoint {
  /// Extension point identifier used in plugin manifests. Use up to 128 lowercase ASCII letters, digits, dots,
  /// underscores, or hyphens, starting with a letter or digit.
  std::string_view id;
};

/// @brief Generated move-only handle that invokes operations on one worker instance.
/// @tparam Interface Interface exposed by the handle.
/// @see PluginManager::OpenExtension()
template<class Interface>
class ExtensionHandle;

} // namespace pluginkit
