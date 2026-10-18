
#ifndef VDATOPICS_HPP
#define VDATOPICS_HPP

#pragma once

#include <optional>
#include <string>

inline const std::string kTopicPrefix = "amr/v3";

inline std::string robot_topic(const std::string& manufacturer, const std::string& serial_number,
                               const std::string& kind) {
    return kTopicPrefix + "/" + manufacturer + "/" + serial_number + "/" + kind;
}

struct ParsedTopic {
    std::string manufacturer;
    std::string serial_number;
    std::string kind;   // state, connection, factsheet ...
};

inline std::optional<ParsedTopic> parse_topic(const std::string& topic) {
    std::vector<std::string> parts;
    std::string part;
    for (char c : topic) {
        if (c == '/') {
            parts.push_back(part);
            part.clear();
        } else {
            part += c;
        }
    }
    parts.push_back(part);

    if (parts.size() != 5 || parts[0] + "/" + parts[1] != kTopicPrefix) {
        return std::nullopt;
    }
    return ParsedTopic{parts[2], parts[3], parts[4]};
}


#endif //VDATOPICS_HPP
