#include "core/sda/revoke.hpp"

#include <chrono>
#include <map>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/crypto/rng.hpp"
#include "core/http/client.hpp"
#include "core/http/url.hpp"
#include "core/json_num.hpp"
#include "core/log.hpp"
#include "core/sda/totp.hpp"
#include "core/time_aligner.hpp"

namespace sam::sda {

using json = nlohmann::json;

namespace {

constexpr char kApiHost[] = "https://api.steampowered.com";

std::string access_token(const core::Account& a) {
    return std::string(a.access_token.begin(), a.access_token.end());
}

http::Response post_two_factor(const std::string& method,
                                const std::string& access,
                                const std::map<std::string, std::string>& fields) {
    http::Request req;
    req.method = http::Method::Post;
    req.url = std::string{kApiHost} + "/ITwoFactorService/" + method +
              "/v1/?access_token=" + http::url_encode(access);
    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    req.body = http::form_encode(fields);
    return http::request(req);
}

core::SteamGuardAccount sda_from_response(const json& r, const std::string& device_id) {
    core::SteamGuardAccount s;
    s.shared_secret   = r.value("shared_secret", "");
    s.serial_number   = r.value("serial_number", "");
    s.revocation_code = r.value("revocation_code", "");
    s.uri             = r.value("uri", "");
    s.server_time     = r.contains("server_time") ? parse_json_i64(r["server_time"], 0) : 0;
    s.account_name    = r.value("account_name", "");
    s.token_gid       = r.value("token_gid", "");
    s.identity_secret = r.value("identity_secret", "");
    s.secret_1        = r.value("secret_1", "");
    s.status          = r.contains("status") ? static_cast<int>(parse_json_i64(r["status"], 1)) : 1;
    s.device_id       = device_id;
    s.fully_enrolled  = false;
    return s;
}

}  // namespace

AddAuthenticatorResult add_authenticator(const core::Account& a) {
    AddAuthenticatorResult out;

    const std::string token = access_token(a);
    if (token.empty() || a.steam_id_64 == 0) {
        return out;
    }

    const std::string device_id = a.sda ? a.sda->device_id : crypto::random_device_id();

    std::map<std::string, std::string> fields{
        {"steamid", std::to_string(a.steam_id_64)},
        {"authenticator_type", "1"},
        {"device_identifier", device_id},
        {"sms_phone_id", "1"},
        {"version", "2"},
    };

    auto resp = post_two_factor("AddAuthenticator", token, fields);
    out.raw_response = resp.body;
    if (resp.status != 200) {
        SAM_LOG_WARN("AddAuthenticator: http {}", resp.status);
        return out;
    }

    try {
        const auto j = json::parse(resp.body);
        if (!j.contains("response")) return out;
        const auto& r = j["response"];
        out.status_code = r.value("status", 0);
        if (out.status_code == 1) {
            out.sda = sda_from_response(r, device_id);
            out.ok = true;
        }
    } catch (const std::exception& ex) {
        SAM_LOG_WARN("AddAuthenticator: parse failed: {}", ex.what());
    }
    return out;
}

FinalizeResult finalize_add(const core::Account& a, const std::string& sms_code) {
    FinalizeResult out;

    const std::string token = access_token(a);
    if (token.empty() || !a.sda.has_value()) {
        out.error = "no access token or no Steam Guard attached";
        return out;
    }

    const auto& sda = *a.sda;

    bool resynced = false;
    for (int tries = 0; tries < 10; ++tries) {
        const std::int64_t when = time_aligner::aligned_now();
        const std::string code = generate_code(sda.shared_secret, when);

        std::map<std::string, std::string> fields{
            {"steamid", std::to_string(a.steam_id_64)},
            {"authenticator_code", code},
            {"authenticator_time", std::to_string(when)},
            {"activation_code", sms_code},
            {"validate_sms_code", "1"},
        };

        auto resp = post_two_factor("FinalizeAddAuthenticator", token, fields);
        if (resp.status != 200) {
            out.error = "http " + std::to_string(resp.status);
            return out;
        }

        bool success = false;
        bool want_more = false;
        try {
            const auto j = json::parse(resp.body);
            if (!j.contains("response")) {
                out.error = "no response field";
                return out;
            }
            const auto& r = j["response"];
            out.status_code = r.contains("status")
                ? static_cast<int>(parse_json_i64(r["status"], 0)) : 0;
            success   = r.value("success", false);
            want_more = r.value("want_more", false);
        } catch (const std::exception& ex) {
            out.error = ex.what();
            return out;
        }

        SAM_LOG_INFO("FinalizeAddAuthenticator: try {} status={} success={} want_more={}",
                     tries, out.status_code, success, want_more);

        if (out.status_code == 89) {
            out.needs_retry = true;
            return out;
        }
        if (out.status_code == 88) {

            if (!resynced) {
                resynced = true;
                (void)time_aligner::sync_now();
                continue;
            }
            std::this_thread::sleep_for(std::chrono::seconds(seconds_remaining(when) + 1));
            continue;
        }
        if (want_more) {
            std::this_thread::sleep_for(std::chrono::seconds(seconds_remaining(when) + 1));
            continue;
        }
        if (success || out.status_code == 1) {
            out.ok = true;
            return out;
        }

        out.error = "status " + std::to_string(out.status_code);
        return out;
    }

    out.error = "Steam did not confirm the authenticator after several tries. "
                "Check that your system clock is accurate.";
    return out;
}

RemoveResult remove_authenticator(const core::Account& a, int scheme) {
    RemoveResult out;
    if (!a.sda.has_value()) {
        out.error = "no Steam Guard attached";
        return out;
    }

    const std::string token = access_token(a);
    if (token.empty()) {
        out.error = "no access token";
        return out;
    }

    std::map<std::string, std::string> fields{
        {"revocation_code", a.sda->revocation_code},
        {"revocation_reason", "1"},
        {"steamguard_scheme", std::to_string(scheme)},
    };

    auto resp = post_two_factor("RemoveAuthenticator", token, fields);
    if (resp.status != 200) {
        out.error = "http " + std::to_string(resp.status);
        return out;
    }

    try {
        const auto j = json::parse(resp.body);
        if (!j.contains("response")) {
            out.error = "no response field";
            return out;
        }
        const auto& r = j["response"];
        out.status_code = r.value("status", 0);
        out.ok = r.value("success", false) || out.status_code == 1;
        if (!out.ok) {
            out.error = "status " + std::to_string(out.status_code);
        }
    } catch (const std::exception& ex) {
        out.error = ex.what();
    }
    return out;
}

TwoFactorStatus query_two_factor_status(const core::Account& a) {
    TwoFactorStatus out;

    const std::string token = access_token(a);
    if (token.empty() || a.steam_id_64 == 0) {
        out.error = "no access token";
        return out;
    }

    std::map<std::string, std::string> fields{
        {"steamid", std::to_string(a.steam_id_64)},
    };

    auto resp = post_two_factor("QueryStatus", token, fields);
    out.raw_response = resp.body;
    if (resp.status != 200) {
        SAM_LOG_WARN("QueryStatus: http {}", resp.status);
        out.error = "http " + std::to_string(resp.status);
        return out;
    }

    try {
        const auto j = json::parse(resp.body);
        if (!j.contains("response")) {
            out.error = "no response field";
            return out;
        }
        const auto& r = j["response"];
        out.status_code        = r.value("status", 0);
        out.authenticator_type = r.value("authenticator_type", 0);
        out.steamguard_scheme  = r.value("steamguard_scheme", 0);
        out.state              = r.value("state", 0);
        out.token_gid          = r.value("token_gid", std::string{});
        out.device_identifier  = r.value("device_identifier", std::string{});
        out.ok = true;
    } catch (const std::exception& ex) {
        SAM_LOG_WARN("QueryStatus: parse failed: {}", ex.what());
        out.error = ex.what();
    }
    return out;
}

}  // namespace sam::sda
