#include "core/steam_cm/web_token.hpp"

#include <chrono>
#include <string>
#include <utility>

#include "core/log.hpp"
#include "core/steam_auth/gen/steammessages_auth.steamclient.pb.h"
#include "core/steam_cm/cm_session.hpp"
#include "core/steam_cm/emsg.hpp"

namespace sam::steam_cm {

namespace {

constexpr char kMethod[] = "Authentication.GenerateAccessTokenForApp#1";
constexpr int kEResultOK = 1;

constexpr int kAttempts = 3;
constexpr int kReplyTimeoutSeconds = 15;

}  // namespace

std::optional<MintedAccessToken> mint_web_access_token(const crypto::SecureString& refresh_token,
                                                       std::uint64_t steam_id,
                                                       std::string& error) {
    if (refresh_token.empty() || steam_id == 0) {
        error = "no refresh token or Steam ID";
        return std::nullopt;
    }
    const std::string token(refresh_token.begin(), refresh_token.end());

    for (int attempt = 1; attempt <= kAttempts; ++attempt) {
        CmSession cm;
        if (!cm.connect(token, steam_id, error)) {
            SAM_LOG_INFO("web-token: attempt {}/{} CM logon failed for {}: {}",
                         attempt, kAttempts, steam_id, error);
            cm.disconnect();
            continue;
        }

        ::CAuthentication_AccessToken_GenerateForApp_Request req;
        req.set_refresh_token(token);
        req.set_steamid(steam_id);

        const std::uint64_t jobid = cm.next_jobid();
        if (!cm.send_service_method(kMethod, req, jobid)) {
            error = "could not send the access-token request";
            cm.disconnect();
            continue;
        }

        std::optional<MintedAccessToken> minted;
        bool replied = false;
        int reply_eresult = 0;
        auto on_message = [&](std::uint32_t emsg, const ::CMsgProtoBufHeader& header,
                              const std::string& body) {
            if (emsg != EMsg::ServiceMethodResponse || header.jobid_target() != jobid) return;
            replied = true;
            reply_eresult = header.eresult();
            if (reply_eresult != kEResultOK) return;

            ::CAuthentication_AccessToken_GenerateForApp_Response resp;
            if (!resp.ParseFromString(body)) {
                SAM_LOG_WARN("web-token: could not parse the response for {} ({} bytes)",
                             steam_id, body.size());
                return;
            }
            if (resp.access_token().empty()) {
                SAM_LOG_WARN("web-token: response for {} carried no access token", steam_id);
                return;
            }

            MintedAccessToken out;
            out.access_token = crypto::make_secure(resp.access_token());
            if (!resp.refresh_token().empty()) {

                SAM_LOG_ERROR("web-token: Steam rotated the refresh token for {} despite "
                              "renewal being off; handing back the replacement", steam_id);
                out.rotated_refresh_token = crypto::make_secure(resp.refresh_token());
            }
            minted = std::move(out);
        };

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(kReplyTimeoutSeconds);
        while (!replied && std::chrono::steady_clock::now() < deadline) {
            if (!cm.pump(500, on_message)) break;
        }
        cm.disconnect();

        if (minted) {
            SAM_LOG_INFO("web-token: minted a web access token for {} on attempt {}",
                         steam_id, attempt);
            return minted;
        }
        if (replied) {

            error = "Steam refused the access-token request (eresult " +
                    std::to_string(reply_eresult) + ")";
            SAM_LOG_WARN("web-token: {} for {}", error, steam_id);
            return std::nullopt;
        }
        error = "no response to the access-token request";
        SAM_LOG_INFO("web-token: attempt {}/{} timed out for {}", attempt, kAttempts, steam_id);
    }

    SAM_LOG_WARN("web-token: could not mint a web access token for {}: {}", steam_id, error);
    return std::nullopt;
}

}  // namespace sam::steam_cm
