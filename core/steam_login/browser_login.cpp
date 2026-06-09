#include "core/steam_login/browser_login.hpp"

#include <string>

namespace sam::steam_login {

namespace {

// Escapes a value for use inside an HTML double-quoted attribute.
std::string html_attr_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (const char c : in) {
        switch (c) {
            case '&': out += "&amp;";  break;
            case '"': out += "&quot;"; break;
            case '<': out += "&lt;";   break;
            case '>': out += "&gt;";   break;
            default:  out += c;        break;
        }
    }
    return out;
}

// Escapes a value for use inside a single-quoted JavaScript string literal.
std::string js_string_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (const char c : in) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'";  break;
            case '<':  out += "\\x3c"; break;  // avoid closing the <script>
            case '>':  out += "\\x3e"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

}  // namespace

std::string build_login_html(const std::vector<TransferTarget>& targets,
                             std::uint64_t steam_id_64,
                             const std::string& final_url) {
    const std::string steamid = std::to_string(steam_id_64);

    std::string html;
    html += "<!doctype html>\n";
    html += "<html><head><meta charset=\"utf-8\"><title>Signing in...</title></head>\n";
    html += "<body style=\"font-family:sans-serif;background:#1b2838;color:#c7d5e0\">\n";
    html += "<p style=\"margin:24px\">Signing in to Steam...</p>\n";

    // One hidden iframe + form per transfer target. Posting into the iframe sets
    // that domain's login cookies without navigating the top window away.
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const std::string idx    = std::to_string(i);
        const std::string action = html_attr_escape(targets[i].url);
        const std::string nonce  = html_attr_escape(targets[i].nonce);
        const std::string auth   = html_attr_escape(targets[i].auth);
        html += "<iframe name=\"t" + idx + "\" style=\"display:none\"></iframe>\n";
        html += "<form id=\"f" + idx + "\" method=\"POST\" action=\"" + action +
                "\" target=\"t" + idx + "\">\n";
        html += "<input type=\"hidden\" name=\"nonce\" value=\"" + nonce + "\">\n";
        html += "<input type=\"hidden\" name=\"auth\" value=\"" + auth + "\">\n";
        html += "<input type=\"hidden\" name=\"steamID\" value=\"" + steamid + "\">\n";
        html += "</form>\n";
    }

    // Submit every form, then send the top window to the profile. We navigate on
    // a short timer (the cross-origin iframe loads aren't readable), which is
    // ample for the settoken POSTs to set their cookies.
    html += "<script>\n";
    html += "var N=" + std::to_string(targets.size()) + ";\n";
    html += "for(var i=0;i<N;i++){try{document.getElementById('f'+i).submit();}catch(e){}}\n";
    html += "setTimeout(function(){location.href='" + js_string_escape(final_url) + "';},2500);\n";
    html += "</script>\n";
    html += "</body></html>\n";
    return html;
}

}  // namespace sam::steam_login
