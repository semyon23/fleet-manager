#include "routing/Router.hpp"

#include <algorithm>
#include <boost/graph/compressed_sparse_row_graph.hpp>
#include <boost/graph/dijkstra_shortest_paths.hpp>
#include <boost/graph/reverse_graph.hpp>
#include <boost/graph/strong_components.hpp>
#include <boost/property_map/property_map.hpp>
#include <functional>
#include <limits>

namespace routing {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr uint32_t kNone = std::numeric_limits<uint32_t>::max();

struct EdgeData {
    double seconds = 0;   // время проезда
    uint32_t edge = 0;    // номер ребра в Graph::edges
};

// CSR: соседи узла лежат подряд в одном массиве. bidirectionalS хранит и входящие рёбра —
// для обратной волны times_to без копии графа
using Csr = boost::compressed_sparse_row_graph<boost::bidirectionalS, boost::no_property, EdgeData,
                                               boost::no_property, uint32_t, uint32_t>;

struct Stop {};   // остановить поиск: BGL прерывается только исключением из visitor'а

}  // namespace

struct Router::Impl {
    Csr graph;
    std::vector<uint32_t> edge_from;   // откуда ребро — чтобы пройти маршрут назад от цели
    std::vector<uint32_t> component;
    uint32_t component_count = 0;

    // Буферы поиска на весь граф. Заполнены «бесконечностью» один раз; после поиска
    // сбрасываются только узлы, до которых дошла волна (touched), — поиск рядом не трогает весь склад
    std::vector<double> dist;
    std::vector<boost::default_color_type> color;
    std::vector<uint32_t> pred_edge;   // ребро, которым пришли в узел
    std::vector<char> settled;         // время узла точное: извлечён из очереди
    std::vector<char> mark;            // цели route / роботы times_to
    std::vector<uint32_t> touched;

    // route: запомнить ребро, которым пришли; стоп на первой извлечённой цели
    struct RouteVisitor : boost::default_dijkstra_visitor {
        Impl* s;
        uint32_t* reached;
        template <class G> void discover_vertex(uint32_t v, const G&) const { s->touched.push_back(v); }
        template <class E, class G> void edge_relaxed(E e, const G& g) const {
            s->pred_edge[target(e, g)] = g[e].edge;
        }
        template <class G> void examine_vertex(uint32_t v, const G&) const {
            if (s->mark[v]) {
                *reached = v;
                throw Stop{};
            }
        }
    };

    // times_to: стоп, когда найдены все нужные роботы или волна ушла дальше предела
    struct TimesVisitor : boost::default_dijkstra_visitor {
        Impl* s;
        size_t* left;
        double max_seconds;
        template <class G> void discover_vertex(uint32_t v, const G&) const { s->touched.push_back(v); }
        template <class G> void examine_vertex(uint32_t v, const G&) const {
            if (s->dist[v] > max_seconds) {
                throw Stop{};   // дальше все узлы ещё дальше
            }
            s->settled[v] = 1;
            if (s->mark[v] && --*left == 0) {
                throw Stop{};
            }
        }
    };

    // Dijkstra без начального заполнения буферов (оно уже сделано) из нескольких стартов сразу
    template <class G, class Visitor>
    void run(const G& g, std::span<const uint32_t> sources, Visitor visitor) {
        for (uint32_t source : sources) {
            dist[source] = 0;
        }
        auto index = get(boost::vertex_index, g);
        try {
            boost::dijkstra_shortest_paths_no_init(
                g, sources.begin(), sources.end(), boost::dummy_property_map(),
                boost::make_iterator_property_map(dist.begin(), index), get(&EdgeData::seconds, g), index,
                std::less<double>(), std::plus<double>(), 0.0, visitor,
                boost::make_iterator_property_map(color.begin(), index));
        } catch (const Stop&) {
        }
    }

    void reset() {
        for (uint32_t v : touched) {
            dist[v] = kInf;
            color[v] = boost::white_color;
            pred_edge[v] = kNone;
            settled[v] = 0;
        }
        touched.clear();
    }
};

Router::Router(const Graph& graph, double default_speed) : impl_(std::make_unique<Impl>()) {
    Impl& s = *impl_;
    const auto nodes = static_cast<uint32_t>(graph.nodes.size());

    std::vector<std::pair<uint32_t, uint32_t>> ends;
    std::vector<EdgeData> data;
    s.edge_from.resize(graph.edges.size());
    for (uint32_t i = 0; i < graph.edges.size(); ++i) {
        const GraphEdge& edge = graph.edges[i];
        s.edge_from[i] = edge.from;
        const double speed = edge.max_speed.value_or(default_speed);
        if (speed <= 0) {
            continue;   // maxSpeed 0 — по ребру не проехать
        }
        ends.emplace_back(edge.from, edge.to);
        data.push_back({edge.length / speed + edge.cost, i});
    }
    s.graph = Csr(boost::edges_are_unsorted_multi_pass, ends.begin(), ends.end(), data.begin(), nodes);

    s.dist.assign(nodes, kInf);
    s.color.assign(nodes, boost::white_color);
    s.pred_edge.assign(nodes, kNone);
    s.settled.assign(nodes, 0);
    s.mark.assign(nodes, 0);
    s.component.assign(nodes, 0);
    if (nodes > 0) {
        s.component_count = static_cast<uint32_t>(boost::strong_components(
            s.graph, boost::make_iterator_property_map(s.component.begin(), get(boost::vertex_index, s.graph))));
    }
}

Router::~Router() = default;
Router::Router(Router&&) noexcept = default;
Router& Router::operator=(Router&&) noexcept = default;

std::optional<Route> Router::route(uint32_t from, std::span<const uint32_t> targets) {
    Impl& s = *impl_;
    if (std::ranges::find(targets, from) != targets.end()) {
        return Route{{from}, {}, 0.0};   // уже на месте
    }
    for (uint32_t target : targets) {
        s.mark[target] = 1;
    }
    uint32_t reached = kNone;
    s.run(s.graph, std::span<const uint32_t>(&from, 1), Impl::RouteVisitor{{}, &s, &reached});

    std::optional<Route> result;
    if (reached != kNone) {
        // По отметкам «откуда пришли» — от цели назад к старту
        Route route;
        route.seconds = s.dist[reached];
        for (uint32_t v = reached; v != from; v = s.edge_from[s.pred_edge[v]]) {
            route.nodes.push_back(v);
            route.edges.push_back(s.pred_edge[v]);
        }
        route.nodes.push_back(from);
        std::ranges::reverse(route.nodes);
        std::ranges::reverse(route.edges);
        result = std::move(route);
    }
    for (uint32_t target : targets) {
        s.mark[target] = 0;
    }
    s.reset();
    return result;
}

std::vector<double> Router::times_to(std::span<const uint32_t> targets, std::span<const uint32_t> sources,
                                     size_t max_found, double max_seconds) {
    Impl& s = *impl_;
    std::vector<double> times(sources.size(), kInf);
    size_t left = 0;   // сколько разных узлов-роботов ещё найти
    for (uint32_t source : sources) {
        if (!s.mark[source]) {
            s.mark[source] = 1;
            ++left;
        }
    }
    left = std::min(left, max_found);
    if (left > 0 && !targets.empty()) {
        // Стрелки развёрнуты: волна от целей приходит к роботу за то же время, что робот — к цели
        s.run(boost::make_reverse_graph(s.graph), targets, Impl::TimesVisitor{{}, &s, &left, max_seconds});
        for (size_t i = 0; i < sources.size(); ++i) {
            if (s.settled[sources[i]]) {
                times[i] = s.dist[sources[i]];
            }
        }
    }
    for (uint32_t source : sources) {
        s.mark[source] = 0;
    }
    s.reset();
    return times;
}

uint32_t Router::component_count() const { return impl_->component_count; }

uint32_t Router::component(uint32_t node) const { return impl_->component[node]; }

}  // namespace routing