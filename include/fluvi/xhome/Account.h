// SPDX-License-Identifier: GPL-3.0-only
// Ported from Veyra (GPL-3.0-only), include/veyra/xbox/Account.h @ c6633121. Changes: namespace, Fluvi data dir, web authorization for wake.
// Signing in to Xbox for home streaming, the way Greenlight does it (unknownskl/greenlight, MIT, via
// xal-node's MSAL flow): a Microsoft device-code sign-in with the xbox.com web client id, then Xbox Live
// user and XSTS tokens, then a streaming token for the "xhome" offering. The user signs in on Microsoft's
// own page (microsoft.com/link); this program never sees the password. Only the refresh token and the
// current access token are kept, DPAPI-protected (CryptProtectData, current user), under
// %LOCALAPPDATA%\Fluvi\xhome. Tokens are never logged.
//
// Unofficial: the client id is Microsoft's, borrowed as every open-source client does; Microsoft can
// change or block this at any time.
//
// Failures throw ServiceError / NetworkError / std::runtime_error (Https.h); WebRtcSession::start and the
// storage helpers report failure by return value instead.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "fluvi/xhome/Https.h"

namespace fluvi::xhome {

inline constexpr const char* kMsalClientId = "1f907974-e22b-4810-a9de-d9647380c97e";

struct DeviceCode {
    std::string userCode;          // shown to the user
    std::string deviceCode;        // secret, polled with
    std::string verificationUri;   // https://www.microsoft.com/link
    int expiresInSeconds = 900;
    int intervalSeconds = 5;
};

enum class SignInPoll { Pending, SlowDown, Done, Expired, Declined, Failed };

struct StreamingAccess {
    std::string host;              // region host of the xhome offering, without scheme
    std::string gsToken;           // bearer token for the streaming API
    std::string regionName;
    int64_t expiresAt = 0;         // unix seconds
};

// The persisted part of a sign-in.
struct SavedTokens {
    std::string refreshToken;
    std::string accessToken;
    int64_t accessExpiresAt = 0;
    std::string userHash;          // uhs, for display / diagnostics only
};

// %LOCALAPPDATA%\Fluvi\xhome; the FLUVI_XHOME_DATA environment variable overrides it (tests).
std::filesystem::path dataDirectory();
// Tokens on disk: JSON through CryptProtectData (current user, app-specific entropy), written to a
// temp file and renamed. loadTokens returns nullopt for a missing, foreign or corrupt file.
std::optional<SavedTokens> loadTokens(const std::filesystem::path& file);
bool saveTokens(const std::filesystem::path& file, const SavedTokens& tokens);

// The XErr codes Xbox Live answers sign-in problems with, as a sentence for the user (empty if unknown).
std::wstring describeXboxError(const std::string& responseBody);

class Account {
public:
    Account(std::shared_ptr<HttpsTransport> transport, std::filesystem::path tokenFile);

    bool signedIn() const;
    void signOut();

    // Device-code sign-in: show userCode and verificationUri, then poll every intervalSeconds.
    DeviceCode beginSignIn();
    SignInPoll pollSignIn(const DeviceCode& code, std::string* detail = nullptr);

    // A streaming token for the home (console) offering. Refreshes everything that has expired.
    StreamingAccess streamingAccess();
    // The token the session's /connect step wants (MSAL "console transfer" token).
    std::string transferToken();
    // "XBL3.0 x=<uhs>;<XSTS token>" for the Xbox Live web services (relying party http://xboxlive.com),
    // what xccs.xboxlive.com (remote management, wake) takes as Authorization.
    std::string webAuthorization();

    // Clock injection for tests (unix seconds).
    void setClock(std::function<int64_t()> clock) { clock_ = std::move(clock); }

private:
    std::string userAccessToken();
    std::string xstsUserToken();
    void store();

    std::shared_ptr<HttpsTransport> http_;
    std::filesystem::path file_;
    mutable std::mutex mutex_;
    std::optional<SavedTokens> tokens_;
    std::string userToken_;          // user.auth.xboxlive.com
    int64_t userTokenExpiresAt_ = 0;
    std::optional<StreamingAccess> streaming_;
    std::string webAuth_;            // cached webAuthorization()
    int64_t webAuthExpiresAt_ = 0;
    std::function<int64_t()> clock_;
};

} // namespace fluvi::xhome
