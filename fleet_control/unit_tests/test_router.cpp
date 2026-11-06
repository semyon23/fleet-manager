// Тесты routing::Router (M-3b).
//
// Как устроены: строим маленькую карту, ищем путь и сравниваем с ответом, посчитанным руками.
// В конце — случайные карты, где ответ Router сравнивается с простым перебором всех пар (Floyd).
//
// Карта в тестах: resolution 1 м, origin 0, высота картинки 100.
// Поэтому узел с u = 10, v = 50 стоит в точке x = 10 м, y = 100 - 50 = 50 м.
// Скорость по умолчанию — 1 м/с, значит время ребра без maxSpeed = его длине в метрах.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "maps/Graph.hpp"
#include "routing/Router.hpp"

using Catch::Matchers::WithinAbs;
using nlohmann::json;

// ================================================================== помощники

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

// Документ карты, как его присылает редактор UI
struct MapDoc {
    json waypoints = json::array();
    json stations = json::array();
    json edges = json::array();

    void node(const std::string& id, double u, double v) {
        waypoints.push_back({{"id", id}, {"u", u}, {"v", v}});
    }

    void station(const std::string& id, double u, double v, const std::vector<std::string>& interaction_nodes) {
        stations.push_back({{"id", id}, {"u", u}, {"v", v}, {"kind", "charge"}, {"interactionNodeIds", interaction_nodes}});
    }

    // Одностороннее ребро from → to. extra — например {{"maxSpeed", 0.5}} или {{"cost", 5}}
    void edge(const std::string& from, const std::string& to, const json& extra = json::object()) {
        json e = {{"id", "e" + std::to_string(edges.size())}, {"from", from}, {"to", to}};
        e.update(extra);
        edges.push_back(e);
    }

    // Двусторонняя дорога = два ребра
    void road(const std::string& a, const std::string& b, const json& extra = json::object()) {
        edge(a, b, extra);
        edge(b, a, extra);
    }

    Graph build() const {
        json body = {{"width", 100},
                     {"height", 100},
                     {"meta", {{"resolution", 1.0}, {"origin", {0, 0, 0}}}},
                     {"waypoints", waypoints},
                     {"stations", stations},
                     {"edges", edges}};
        REQUIRE(validate_map(body).errors.empty());
        return build_graph("test", 1, body);
    }
};

// Номер узла по id
uint32_t id_to_node(const Graph& graph, const std::string& id) {
    auto index = graph.find_node(id);
    REQUIRE(index);
    return *index;
}

// Номера узлов маршрута → их id, чтобы сравнивать с ожидаемым списком
std::vector<std::string> node_ids(const Graph& graph, const std::vector<uint32_t>& nodes) {
    std::vector<std::string> result;
    for (uint32_t n : nodes) {
        result.push_back(graph.nodes[n].id);
    }
    return result;
}

// Путь из from в один узел to
std::optional<routing::Route> find_route(routing::Router& router, const Graph& graph, const std::string& from,
                                         const std::string& to) {
    std::vector<uint32_t> targets = {id_to_node(graph, to)};
    return router.route(id_to_node(graph, from), targets);
}

// Время от каждого робота до цели to, без ограничений
std::vector<double> times_to_node(routing::Router& router, const Graph& graph, const std::string& to,
                                  const std::vector<std::string>& robots) {
    std::vector<uint32_t> targets = {id_to_node(graph, to)};
    std::vector<uint32_t> sources;
    for (const auto& robot : robots) {
        sources.push_back(id_to_node(graph, robot));
    }
    return router.times_to(targets, sources, sources.size(), kInf);
}

// Линия n0 — n1 — … — n9, узлы через 10 м, дороги двусторонние.
// Время между соседями — 10 с, от n0 до nK — K × 10 с.
Graph line_map() {
    MapDoc doc;
    for (int i = 0; i < 10; ++i) {
        doc.node("n" + std::to_string(i), i * 10, 50);
        if (i > 0) {
            doc.road("n" + std::to_string(i - 1), "n" + std::to_string(i));
        }
    }
    return doc.build();
}

}  // namespace

// ================================================================== route: маленькие карты

TEST_CASE("быстрая длинная дорога лучше медленной короткой") {
    //        C
    //      /   \          A → D напрямую: 20 м по 0,5 м/с = 40 с
    //    A ----- D        A → C → D: 14,14 м + 14,14 м по 1 м/с ≈ 28,3 с — быстрее
    MapDoc doc;
    doc.node("A", 0, 50);
    doc.node("C", 10, 40);
    doc.node("D", 20, 50);
    doc.edge("A", "D", {{"maxSpeed", 0.5}});
    doc.edge("A", "C");
    doc.edge("C", "D");
    Graph graph = doc.build();
    routing::Router router(graph, 1.0);

    auto route = find_route(router, graph, "A", "D");

    REQUIRE(route);
    CHECK(node_ids(graph, route->nodes) == std::vector<std::string>{"A", "C", "D"});
    CHECK_THAT(route->seconds, WithinAbs(2 * std::sqrt(200.0), 1e-9));
}

TEST_CASE("скорость по умолчанию действует только там, где нет maxSpeed") {
    //    A ---10 м--- B ---10 м, maxSpeed 0,5--- C
    // По умолчанию 2 м/с: A → B = 5 с, B → C = 20 с, всего 25 с
    MapDoc doc;
    doc.node("A", 0, 50);
    doc.node("B", 10, 50);
    doc.node("C", 20, 50);
    doc.edge("A", "B");
    doc.edge("B", "C", {{"maxSpeed", 0.5}});
    Graph graph = doc.build();
    routing::Router router(graph, 2.0);

    auto route = find_route(router, graph, "A", "C");

    REQUIRE(route);
    CHECK_THAT(route->seconds, WithinAbs(25, 1e-9));
}

TEST_CASE("cost — добавочные секунды на ребре") {
    //      B
    //    /   \  cost 5        Две одинаковые дороги, но через B на 5 с дольше
    //  A       D
    //    \   /
    //      C
    MapDoc doc;
    doc.node("A", 0, 50);
    doc.node("B", 10, 40);
    doc.node("C", 10, 60);
    doc.node("D", 20, 50);
    doc.edge("A", "B");
    doc.edge("B", "D", {{"cost", 5}});
    doc.edge("A", "C");
    doc.edge("C", "D");
    Graph graph = doc.build();
    routing::Router router(graph, 1.0);

    auto route = find_route(router, graph, "A", "D");

    REQUIRE(route);
    CHECK(node_ids(graph, route->nodes) == std::vector<std::string>{"A", "C", "D"});
}

TEST_CASE("одностороннее ребро: туда можно, обратно нет") {
    //    A ----> B
    MapDoc doc;
    doc.node("A", 0, 50);
    doc.node("B", 10, 50);
    doc.edge("A", "B");
    Graph graph = doc.build();
    routing::Router router(graph, 1.0);

    CHECK(find_route(router, graph, "A", "B"));
    CHECK_FALSE(find_route(router, graph, "B", "A"));
}

TEST_CASE("maxSpeed 0 — по ребру не проехать") {
    MapDoc doc;
    doc.node("A", 0, 50);
    doc.node("B", 10, 50);
    doc.edge("A", "B", {{"maxSpeed", 0}});
    Graph graph = doc.build();
    routing::Router router(graph, 1.0);

    CHECK_FALSE(find_route(router, graph, "A", "B"));
}

TEST_CASE("робот уже в точке — маршрут из одного узла, 0 с") {
    MapDoc doc;
    doc.node("A", 0, 50);
    doc.node("B", 10, 50);
    doc.road("A", "B");
    Graph graph = doc.build();
    routing::Router router(graph, 1.0);

    auto route = find_route(router, graph, "A", "A");

    REQUIRE(route);
    CHECK(node_ids(graph, route->nodes) == std::vector<std::string>{"A"});
    CHECK(route->edges.empty());
    CHECK(route->seconds == 0);
}

TEST_CASE("несколько целей — едем к ближайшей") {
    //  far ---40 м--- A ---10 м--- near
    MapDoc doc;
    doc.node("far", 10, 50);
    doc.node("A", 50, 50);
    doc.node("near", 60, 50);
    doc.road("A", "far");
    doc.road("A", "near");
    Graph graph = doc.build();
    routing::Router router(graph, 1.0);

    std::vector<uint32_t> targets = {id_to_node(graph, "far"), id_to_node(graph, "near")};
    auto route = router.route(id_to_node(graph, "A"), targets);

    REQUIRE(route);
    CHECK(graph.nodes[route->nodes.back()].id == "near");
    CHECK_THAT(route->seconds, WithinAbs(10, 1e-9));
}

TEST_CASE("станция без рёбер — едем к ближайшему узлу взаимодействия") {
    //            dock (без рёбер, подъезд с i1 или i2)
    //    A ---- i1 ------------ i2
    MapDoc doc;
    doc.node("A", 0, 50);
    doc.node("i1", 10, 50);
    doc.node("i2", 40, 50);
    doc.station("dock", 25, 30, {"i1", "i2"});
    doc.road("A", "i1");
    doc.road("i1", "i2");
    Graph graph = doc.build();
    routing::Router router(graph, 1.0);

    std::vector<uint32_t> targets = graph.arrival_nodes(id_to_node(graph, "dock"));
    CHECK(node_ids(graph, targets) == std::vector<std::string>{"i1", "i2"});

    auto route = router.route(id_to_node(graph, "A"), targets);

    REQUIRE(route);
    CHECK(graph.nodes[route->nodes.back()].id == "i1");
}

TEST_CASE("станция с рёбрами и обычный узел — едем в сам узел") {
    //    A ---- charger
    MapDoc doc;
    doc.node("A", 0, 50);
    doc.station("charger", 10, 50, {"A"});
    doc.road("A", "charger");
    Graph graph = doc.build();

    uint32_t charger = id_to_node(graph, "charger");
    uint32_t a = id_to_node(graph, "A");
    CHECK(graph.arrival_nodes(charger) == std::vector<uint32_t>{charger});
    CHECK(graph.arrival_nodes(a) == std::vector<uint32_t>{a});
}

// ================================================================== связность

TEST_CASE("две отдельные части склада — пути нет, компонент две") {
    //    A — B          C — D
    MapDoc doc;
    doc.node("A", 0, 50);
    doc.node("B", 10, 50);
    doc.node("C", 50, 50);
    doc.node("D", 60, 50);
    doc.road("A", "B");
    doc.road("C", "D");
    Graph graph = doc.build();
    routing::Router router(graph, 1.0);

    CHECK_FALSE(find_route(router, graph, "A", "D"));
    CHECK(router.component_count() == 2);
    CHECK(router.component(id_to_node(graph, "A")) == router.component(id_to_node(graph, "B")));
    CHECK(router.component(id_to_node(graph, "A")) != router.component(id_to_node(graph, "C")));
}

TEST_CASE("ловушка: в тупик заехать можно, выехать нельзя") {
    //    A — B ----> trap
    MapDoc doc;
    doc.node("A", 0, 50);
    doc.node("B", 10, 50);
    doc.node("trap", 20, 50);
    doc.road("A", "B");
    doc.edge("B", "trap");
    Graph graph = doc.build();
    routing::Router router(graph, 1.0);

    CHECK(find_route(router, graph, "A", "trap"));
    CHECK_FALSE(find_route(router, graph, "trap", "A"));
    CHECK(router.component_count() == 2);
}

TEST_CASE("нормальный склад — одна компонента") {
    Graph graph = line_map();
    routing::Router router(graph, 1.0);

    CHECK(router.component_count() == 1);
}

// ================================================================== times_to

TEST_CASE("times_to: время каждого робота до цели") {
    // Линия n0 … n9, цель n0. Роботы на n3, n9, n5 → 30, 90, 50 с
    Graph graph = line_map();
    routing::Router router(graph, 1.0);

    auto times = times_to_node(router, graph, "n0", {"n3", "n9", "n5"});

    CHECK_THAT(times[0], WithinAbs(30, 1e-9));
    CHECK_THAT(times[1], WithinAbs(90, 1e-9));
    CHECK_THAT(times[2], WithinAbs(50, 1e-9));
}

TEST_CASE("times_to считает по стрелкам робота, а не цели") {
    //    R ----> T        Робот R доедет до T за 10 с. Робот на T до R не доедет
    MapDoc doc;
    doc.node("R", 0, 50);
    doc.node("T", 10, 50);
    doc.edge("R", "T");
    Graph graph = doc.build();
    routing::Router router(graph, 1.0);

    CHECK_THAT(times_to_node(router, graph, "T", {"R"})[0], WithinAbs(10, 1e-9));
    CHECK(times_to_node(router, graph, "R", {"T"})[0] == kInf);
}

TEST_CASE("times_to: max_found — находим только k ближайших") {
    // Цель n0, роботы на n8, n2, n5, n1. Нужны два ближайших: n1 (10 с) и n2 (20 с)
    Graph graph = line_map();
    routing::Router router(graph, 1.0);
    std::vector<uint32_t> targets = {id_to_node(graph, "n0")};
    std::vector<uint32_t> robots = {id_to_node(graph, "n8"), id_to_node(graph, "n2"), id_to_node(graph, "n5"),
                                    id_to_node(graph, "n1")};

    auto times = router.times_to(targets, robots, 2, kInf);

    CHECK(times[0] == kInf);                       // n8 — не искали
    CHECK_THAT(times[1], WithinAbs(20, 1e-9));     // n2
    CHECK(times[2] == kInf);                       // n5 — не искали
    CHECK_THAT(times[3], WithinAbs(10, 1e-9));     // n1
}

TEST_CASE("times_to: max_seconds — дальше предела не ищем") {
    // Цель n0, предел 45 с. n2 — 20 с, найден; n7 — 70 с, дальше предела
    Graph graph = line_map();
    routing::Router router(graph, 1.0);
    std::vector<uint32_t> targets = {id_to_node(graph, "n0")};
    std::vector<uint32_t> robots = {id_to_node(graph, "n2"), id_to_node(graph, "n7")};

    auto times = router.times_to(targets, robots, robots.size(), 45);

    CHECK_THAT(times[0], WithinAbs(20, 1e-9));
    CHECK(times[1] == kInf);
}

TEST_CASE("times_to: два робота в одном узле и робот уже на цели") {
    Graph graph = line_map();
    routing::Router router(graph, 1.0);

    auto times = times_to_node(router, graph, "n0", {"n4", "n4", "n0"});

    CHECK_THAT(times[0], WithinAbs(40, 1e-9));
    CHECK_THAT(times[1], WithinAbs(40, 1e-9));
    CHECK(times[2] == 0);
}

TEST_CASE("times_to: несколько целей — время до ближайшей") {
    // Цели n0 и n9, робот на n7: до n9 — 20 с, до n0 — 70 с
    Graph graph = line_map();
    routing::Router router(graph, 1.0);
    std::vector<uint32_t> targets = {id_to_node(graph, "n0"), id_to_node(graph, "n9")};
    std::vector<uint32_t> robots = {id_to_node(graph, "n7")};

    auto times = router.times_to(targets, robots, 1, kInf);

    CHECK_THAT(times[0], WithinAbs(20, 1e-9));
}

// ================================================================== буферы между запросами

TEST_CASE("после множества запросов ответы те же, что у нового Router") {
    // Router сбрасывает после поиска только затронутые узлы. Если что-то не сбросилось,
    // следующий поиск ответит иначе, чем Router, у которого не было ни одного поиска
    Graph graph = line_map();
    routing::Router used(graph, 1.0);
    std::vector<uint32_t> everyone;
    for (uint32_t i = 0; i < graph.nodes.size(); ++i) {
        everyone.push_back(i);
    }

    for (uint32_t from = 0; from < graph.nodes.size(); ++from) {
        for (uint32_t to = 0; to < graph.nodes.size(); ++to) {
            std::vector<uint32_t> targets = {to};
            routing::Router fresh(graph, 1.0);

            CHECK(used.route(from, targets)->nodes == fresh.route(from, targets)->nodes);
            CHECK(used.times_to(targets, everyone, 3, 25) == fresh.times_to(targets, everyone, 3, 25));
        }
    }
}

// ================================================================== случайные карты против Floyd

namespace {

// Время ребра — по той же формуле, что в Router: длина / скорость + cost
double edge_seconds(const GraphEdge& edge, double default_speed) {
    double speed = edge.max_speed.value_or(default_speed);
    if (speed <= 0) {
        return kInf;
    }
    return edge.length / speed + edge.cost;
}

// Случайная карта: 50–150 узлов, втрое больше дорог; среди них односторонние,
// медленные, со штрафом и закрытые. seed — чтобы упавший тест можно было повторить
Graph random_map(unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> chance(0, 1);
    const int nodes = std::uniform_int_distribution<int>(50, 150)(rng);
    std::uniform_int_distribution<int> any_node(0, nodes - 1);
    std::uniform_int_distribution<int> coord(0, 99);

    MapDoc doc;
    for (int i = 0; i < nodes; ++i) {
        doc.node("n" + std::to_string(i), coord(rng), coord(rng));
    }
    for (int k = 0; k < nodes * 3; ++k) {
        std::string a = "n" + std::to_string(any_node(rng));
        std::string b = "n" + std::to_string(any_node(rng));
        if (a == b) {
            continue;
        }
        json extra = json::object();
        double kind = chance(rng);
        if (kind < 0.2) {
            extra["maxSpeed"] = 0.3 + chance(rng);   // медленная
        } else if (kind < 0.25) {
            extra["maxSpeed"] = 0;                   // закрытая
        }
        if (chance(rng) < 0.2) {
            extra["cost"] = std::round(chance(rng) * 20);
        }
        if (chance(rng) < 0.7) {
            doc.road(a, b, extra);
        } else {
            doc.edge(a, b, extra);                   // односторонняя
        }
    }
    return doc.build();
}

// Floyd: время между всеми парами узлов полным перебором. Медленно (n³), зато ошибиться негде:
// best[i][j] улучшается через каждый промежуточный узел k
std::vector<std::vector<double>> floyd(const Graph& graph, double default_speed) {
    const size_t n = graph.nodes.size();
    std::vector<std::vector<double>> best(n, std::vector<double>(n, kInf));
    for (size_t i = 0; i < n; ++i) {
        best[i][i] = 0;
    }
    for (const auto& edge : graph.edges) {
        best[edge.from][edge.to] = std::min(best[edge.from][edge.to], edge_seconds(edge, default_speed));
    }
    for (size_t k = 0; k < n; ++k) {
        for (size_t i = 0; i < n; ++i) {
            for (size_t j = 0; j < n; ++j) {
                best[i][j] = std::min(best[i][j], best[i][k] + best[k][j]);
            }
        }
    }
    return best;
}

// Маршрут правильный: рёбра идут друг за другом, а их время в сумме — seconds
void check_route_is_connected(const Graph& graph, const routing::Route& route, double default_speed) {
    REQUIRE(route.edges.size() + 1 == route.nodes.size());
    double sum = 0;
    for (size_t i = 0; i < route.edges.size(); ++i) {
        const GraphEdge& edge = graph.edges[route.edges[i]];
        CHECK(edge.from == route.nodes[i]);
        CHECK(edge.to == route.nodes[i + 1]);
        sum += edge_seconds(edge, default_speed);
    }
    CHECK_THAT(sum, WithinAbs(route.seconds, 1e-6));
}

}  // namespace

TEST_CASE("случайные карты: route совпадает с Floyd") {
    for (unsigned seed = 1; seed <= 30; ++seed) {
        CAPTURE(seed);
        Graph graph = random_map(seed);
        routing::Router router(graph, 1.0);
        auto best = floyd(graph, 1.0);

        for (uint32_t from = 0; from < graph.nodes.size(); from += 7) {
            for (uint32_t to = 0; to < graph.nodes.size(); to += 5) {
                CAPTURE(from, to);
                std::vector<uint32_t> targets = {to};
                auto route = router.route(from, targets);

                if (best[from][to] == kInf) {
                    CHECK_FALSE(route);
                } else {
                    REQUIRE(route);
                    CHECK_THAT(route->seconds, WithinAbs(best[from][to], 1e-6));
                    check_route_is_connected(graph, *route, 1.0);
                }
            }
        }
    }
}

TEST_CASE("случайные карты: times_to совпадает с Floyd") {
    for (unsigned seed = 1; seed <= 30; ++seed) {
        CAPTURE(seed);
        Graph graph = random_map(seed);
        routing::Router router(graph, 1.0);
        auto best = floyd(graph, 1.0);
        std::vector<uint32_t> everyone;
        for (uint32_t i = 0; i < graph.nodes.size(); ++i) {
            everyone.push_back(i);
        }

        for (uint32_t to = 0; to < graph.nodes.size(); to += 11) {
            std::vector<uint32_t> targets = {to};
            auto times = router.times_to(targets, everyone, everyone.size(), kInf);

            for (uint32_t robot = 0; robot < graph.nodes.size(); ++robot) {
                CAPTURE(to, robot);
                if (best[robot][to] == kInf) {
                    CHECK(times[robot] == kInf);
                } else {
                    CHECK_THAT(times[robot], WithinAbs(best[robot][to], 1e-6));
                }
            }
        }
    }
}