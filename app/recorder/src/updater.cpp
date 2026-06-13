#include "updater.h"
#include "version.h"

#include <winsparkle/winsparkle.h>

namespace updater {

namespace {

// Public half of the WinSparkle EdDSA signing key. The private half lives in
// the CI secret WINSPARKLE_ED_PRIVATE_KEY and signs each released installer
// (scripts/generate-appcast.ps1). Generate the pair once with WinSparkle's
// generate_keys tool and paste the base64 public key here.
constexpr const char* kEdDsaPublicKey = "";

constexpr const char* kAppcastUrl =
    "https://github.com/fraa2a/Monolith-releases/releases/latest/download/appcast.xml";

bool g_initialized = false;

} // namespace

void init(bool auto_check_enabled)
{
    if (g_initialized) return;

    win_sparkle_set_appcast_url(kAppcastUrl);
    win_sparkle_set_app_details(L"Monolith", L"Monolith",
                                MONOLITH_VERSION_WSTRING);

    // Without a public key WinSparkle still runs but cannot verify update
    // signatures; releases stay unsigned until the key pair exists.
    if (kEdDsaPublicKey[0] != '\0')
        win_sparkle_set_eddsa_public_key(kEdDsaPublicKey);

    win_sparkle_set_automatic_check_for_updates(auto_check_enabled ? 1 : 0);
    win_sparkle_init();
    g_initialized = true;
}

void set_auto_check(bool enabled)
{
    if (!g_initialized) return;
    win_sparkle_set_automatic_check_for_updates(enabled ? 1 : 0);
}

void check_now()
{
    if (!g_initialized) return;
    win_sparkle_check_update_with_ui();
}

void shutdown()
{
    if (!g_initialized) return;
    win_sparkle_cleanup();
    g_initialized = false;
}

} // namespace updater
