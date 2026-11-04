#include "MapStore.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>

namespace dbv1 = fms::database::v1;
using Control = dbv1::ControlStorage::Stub;

constexpr auto kRetryDelay = std::chrono::seconds(2);

MapStore::MapStore(asio::io_context& io_context, Services& services)
    : io_context_(io_context), services_(services), retry_timer_(io_context) {}

void MapStore::start() {
    asio::co_spawn(io_context_, load(), [](std::exception_ptr error) {
        if (!error) return;
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& e) {
            spdlog::error("[map_store] coroutine failed: {}", e.what());
        }
    });
}

void MapStore::stop() {
    stopped_ = true;
    retry_timer_.cancel();
}

const Graph* MapStore::find(const std::string& map_id) const {
    auto it = graphs_.find(map_id);
    return it == graphs_.end() ? nullptr : &it->second;
}

void MapStore::put(Graph graph) {
    std::string map_id = graph.map_id;
    graphs_.insert_or_assign(std::move(map_id), std::move(graph));
}

void MapStore::remove(const std::string& map_id) {
    graphs_.erase(map_id);
}

asio::awaitable<void> MapStore::load() {
    bool warned = false;
    while (!stopped_) {
        dbv1::ListMapsRequest request;
        dbv1::ListMapsResponse response;
        grpc::Status status = co_await services_.call<&Control::PrepareAsyncListMaps>(*services_.control, request, response);
        if (status.ok()) {
            for (const auto& map : response.maps()) {
                add_record(map);
            }
            loaded_ = true;
            spdlog::info("[map_store] {} of {} maps loaded", graphs_.size(), response.maps_size());
            co_return;
        }
        if (!warned) {   // база ещё не поднялась — не спамить каждые 2 с
            spdlog::warn("[map_store] cannot load maps: {}; retrying", status.error_message());
            warned = true;
        }
        retry_timer_.expires_after(kRetryDelay);
        co_await retry_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
    }
}

// Карту с ошибками (например, записанную в базу в обход control) UI видит, но маршрутов по ней нет
void MapStore::add_record(const dbv1::MapRecord& map) {
    auto body = nlohmann::json::parse(map.body_json(), nullptr, false);
    MapCheck check = validate_map(body);
    if (!check.errors.empty()) {
        spdlog::warn("[map_store] map {} v{} is not usable for routes: {}", map.map_id(), map.version(),
                     check.errors.front());
        return;
    }
    put(build_graph(map.map_id(), map.version(), body));
}
