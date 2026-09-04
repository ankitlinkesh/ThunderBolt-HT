#include <thunderbolt/api/BuildInfo.hpp>

#define TB_STRINGIFY_(x) #x
#define TB_STRINGIFY(x)  TB_STRINGIFY_(x)

namespace thunderbolt {
namespace {

// Compiler identification, resolved at compile time.
#if defined(_MSC_VER)
constexpr std::string_view kCompiler = "MSVC " TB_STRINGIFY(_MSC_FULL_VER);
#elif defined(__clang__)
constexpr std::string_view kCompiler = "Clang " __clang_version__;
#elif defined(__GNUC__)
constexpr std::string_view kCompiler = "GCC " __VERSION__;
#else
constexpr std::string_view kCompiler = "unknown";
#endif

// ASan detection differs per compiler; MSVC defines __SANITIZE_ADDRESS__ under
// /fsanitize=address, Clang exposes it through __has_feature.
#if defined(__SANITIZE_ADDRESS__)
constexpr bool kAsan = true;
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
constexpr bool kAsan = true;
#  else
constexpr bool kAsan = false;
#  endif
#else
constexpr bool kAsan = false;
#endif

constexpr BuildInfo kInfo{
    /*version*/           THUNDERBOLT_VERSION_STRING,
    /*compiler*/          kCompiler,
    /*configuration*/     THUNDERBOLT_BUILD_CONFIG,
    /*cpp_standard*/      TB_STRINGIFY(__cplusplus),
    /*debug_assertions*/  THUNDERBOLT_DEBUG != 0,
    /*address_sanitizer*/ kAsan,
};

} // namespace

const BuildInfo& build_info() noexcept { return kInfo; }

} // namespace thunderbolt
