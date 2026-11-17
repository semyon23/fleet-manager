#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "fms_types.pb.h"

struct RoutePart {
    std::vector<std::string> nodes;
    std::vector<std::string> edges;
};

struct RouteParts {
    RoutePart released;
    RoutePart planned;
};

// Пройденное отрезаем по индексу, а не по nodeId:
// один узел может встречаться в маршруте несколько раз.
//
// Последний узел released — первый узел planned.
// Это общий узел соединения двух частей, а не повторное разрешение проезда.
//
// Некорректный прогресс → nullopt.
std::optional<RouteParts> split_route(std::span<const std::string> nodes, std::span<const std::string> edges, const fms::v1::MoveProgress& progress);
