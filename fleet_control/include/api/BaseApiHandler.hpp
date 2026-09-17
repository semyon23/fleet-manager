#pragma once
#ifndef BASEAPIHANDLER_HPP
#define BASEAPIHANDLER_HPP

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

namespace http = boost::beast::http;
using json = nlohmann::json;


class BaseApiHandler {
protected:

    void apply_cors_headers(http::response<http::string_body>& res) const {
        res.set(http::field::access_control_allow_origin, "*");
        res.set(http::field::access_control_allow_methods, "GET, POST, PATCH, DELETE, OPTIONS");
        res.set(http::field::access_control_allow_headers, "Content-Type, Authorization");
        res.set(http::field::access_control_max_age, "86400");
    }


    http::message_generator reply_json(http::status status, unsigned version, const json& body) const {
        http::response<http::string_body> res{status, version};
        res.set(http::field::content_type, "application/json");

        apply_cors_headers(res);

        res.body() = body.dump();
        res.prepare_payload();
        return res;
    }


    http::message_generator reply_error(http::status status, unsigned version, const std::string& error_msg) const {
        return reply_json(status, version, json{{"error", error_msg}});
    }


    std::optional<json> parse_json(const http::request<http::string_body>& req,
                                   std::optional<http::message_generator>& error_out) const {
        try {
            return json::parse(req.body());
        } catch (const json::parse_error&) {
            error_out = reply_error(http::status::bad_request, req.version(), "Invalid JSON format");
            return std::nullopt;
        }
    }
};


#endif //BASEAPIHANDLER_HPP
