// Бенчмарк routing::Router на синтетическом складе (M-3b).
//
// Запускать сборку Release: в Debug шаблоны BGL без оптимизации медленнее в десятки раз,
// и цифры ничего не значат.
//
//   fleet_control_bench                  — склады на 1 000, 10 000 и 50 000 узлов
//   fleet_control_bench 50000            — один размер
//   fleet_control_bench 50000 map.json   — и сохранить склад как карту для UI и симулятора

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "maps/Graph.hpp"
#include "routing/Router.hpp"
#include "warehouse.hpp"

namespace {

using Clock = std::chrono::steady_clock;
constexpr double kInf = std::numeric_limits<double>::infinity();

// Сколько выполняется f, мкс
template <class F>
double micros(F&& f) {
    auto start = Clock::now();
    f();
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

// Среднее, 99-й процентиль и худшее
void print(const char* what, std::vector<double> us) {
    std::ranges::sort(us);
    const double mean = std::accumulate(us.begin(), us.end(), 0.0) / us.size();
    std::printf("  %-36s mean %9.1f   p99 %9.1f   max %9.1f  мкс\n", what, mean, us[us.size() * 99 / 100], us.back());
}

// Склад нужного размера: проходов × узлов в проходе
WarehouseOptions options_for(int nodes) {
    WarehouseOptions options;
    if (nodes <= 1000) {
        options.aisles = 20;
        options.nodes_per_aisle = 50;
    } else if (nodes <= 10000) {
        options.aisles = 50;
        options.nodes_per_aisle = 200;
    } else {
        options.aisles = 100;
        options.nodes_per_aisle = 500;
    }
    return options;
}

void run(int nodes, const std::string& save_path) {
    const WarehouseOptions options = options_for(nodes);
    const nlohmann::json doc = generate_warehouse(options);
    if (!save_path.empty()) {
        std::ofstream(save_path) << doc.dump();
        std::printf("склад сохранён в %s\n", save_path.c_str());
    }

    // ---- построение: то, что происходит при сохранении карты
    Graph graph;
    const double graph_us = micros([&] {
        if (!validate_map(doc).errors.empty()) {
            throw std::runtime_error("generated map is invalid");
        }
        graph = build_graph("bench", 1, doc);
    });
    std::optional<routing::Router> router;
    const double router_us = micros([&] { router.emplace(graph, 1.0); });

    std::printf("\nсклад: %zu узлов, %zu рёбер, компонент связности: %u (должна быть 1)\n", graph.nodes.size(),
                graph.edges.size(), router->component_count());
    std::printf("  проверка и граф карты                 %9.1f мс\n", graph_us / 1000);
    std::printf("  Router (CSR + компоненты)             %9.1f мс\n", router_us / 1000);

    // Узел aA_I имеет номер A * nodes_per_aisle + I (см. warehouse.hpp)
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> any_aisle(0, options.aisles - 1);
    std::uniform_int_distribution<int> any_position(0, options.nodes_per_aisle - 1);
    std::uniform_int_distribution<int> shift(-13, 13);   // 13 узлов × 1,5 м ≈ 20 м
    auto node_at = [&](int aisle, int i) { return static_cast<uint32_t>(aisle * options.nodes_per_aisle + i); };
    auto any_node = [&] { return node_at(any_aisle(rng), any_position(rng)); };

    // ---- route: близкие цели — в том же проходе, в пределах ~20 м
    std::vector<double> near;
    int found = 0;
    for (int q = 0; q < 2000; ++q) {
        const int aisle = any_aisle(rng);
        const int i = any_position(rng);
        const int j = std::clamp(i + shift(rng), 0, options.nodes_per_aisle - 1);
        std::vector<uint32_t> targets = {node_at(aisle, j)};
        near.push_back(micros([&] { found += router->route(node_at(aisle, i), targets).has_value(); }));
    }
    print("route, цель рядом (до 20 м)", near);

    // ---- route: дальние цели — случайная пара через весь склад
    std::vector<double> far;
    for (int q = 0; q < 2000; ++q) {
        const uint32_t from = any_node();
        std::vector<uint32_t> targets = {any_node()};
        far.push_back(micros([&] { found += router->route(from, targets).has_value(); }));
    }
    print("route, цель где угодно", far);

    // ---- times_to: выбор робота из 500
    std::vector<uint32_t> robots(500);
    for (auto& robot : robots) {
        robot = any_node();
    }
    std::vector<double> nearest5;
    std::vector<double> everyone;
    for (int q = 0; q < 500; ++q) {
        std::vector<uint32_t> targets = {any_node()};
        nearest5.push_back(micros([&] { router->times_to(targets, robots, 5, kInf); }));
        everyone.push_back(micros([&] { router->times_to(targets, robots, robots.size(), kInf); }));
    }
    print("times_to, 500 роботов, 5 ближайших", nearest5);
    print("times_to, 500 роботов, все", everyone);

    std::printf("  путей найдено: %d из 4000 (должно быть 4000)\n", found);
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<int> sizes = {1000, 10000, 50000};
    std::string save_path;
    if (argc > 1) {
        sizes = {std::stoi(argv[1])};
    }
    if (argc > 2) {
        save_path = argv[2];
    }
    for (int nodes : sizes) {
        run(nodes, save_path);
    }
}