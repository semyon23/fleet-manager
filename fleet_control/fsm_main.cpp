#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/system/system_error.hpp>

#include <string>

#include <include/api/HttpSession.hpp>

#include "include/objects/RobotObject.hpp"
#include "include/core/RobotsService.hpp"
#include "print"
#include "include/api/ApiController.hpp"
#include "include/core/FleetDispatchClient.hpp"
#include "include/api/RobotApiHandler.hpp"


namespace beast = boost::beast;
namespace http  = beast::http;
namespace asio  = boost::asio;
using json = nlohmann::json;

int main() {
    auto const address = asio::ip::make_address("0.0.0.0");
    auto const port = static_cast<unsigned short>(5000);

    agrpc::GrpcContext grpc_context;
    asio::io_context ioc{1};

    auto grpc_work = asio::make_work_guard(grpc_context);

    ip::tcp::endpoint ep(address, port);
    auto broadcaster = std::make_shared<WebSocketBroadcaster>();


    auto robotService = std::make_shared<fms::RobotsService>();

    auto DispatchClient = std::make_shared<FleetDispatchClient>(grpc_context, "127.0.0.1:5001");
    DispatchClient->run();

    auto robotHandler = std::make_shared<RobotApiHandler>(robotService, DispatchClient);

    auto api = std::make_shared<ApiController>(robotHandler);

    HttpSession session(ep, ioc, std::move(api), broadcaster);

    std::cout << "Server started : " << address << ": "  << port << "\n";


    // asio::steady_timer ticker(ioc, std::chrono::seconds(1));
    // std::function<void(boost::system::error_code)> on_tick = [&](boost::system::error_code ec) {
    //     double robot_x = 10.0;
    //     double robot_y = 5.5;
    //     int battery_level = 100;
    //     if (!ec) {
    //         if (!ec) {
    //             robot_x += 0.2;
    //             robot_y += 0.1;
    //             if (battery_level > 5) battery_level -= 1;
    //
    //             json root_obj = {
    //                 {"type", "state"},
    //                 {"robot_id", "amr-01"},
    //                 {"data", {
    //                     {"x", robot_x},
    //                     {"y", robot_y},
    //                     {"theta", 1.57},
    //                     {"battery", battery_level},
    //                     {"status", "MOVING"}
    //                 }}
    //             };
    //
    //             std::string json_string = root_obj.dump();
    //
    //             broadcaster->broadcast(json_string);
    //
    //             ticker.expires_after(std::chrono::seconds(1));
    //             ticker.async_wait(on_tick);
    //         }
    //     }
    // };
    // ticker.async_wait(on_tick);


    asio::signal_set signals(ioc, SIGINT, SIGTERM);

    signals.async_wait([&](auto, int) {
        DispatchClient->stop();
        //ticker.cancel();
        session.stop();
        grpc_work.reset();

         //grpc_context.stop();
        ioc.stop();
        // DispatchClient.reset();
        std::cout << "Shutdown initiated...\n";
    });


    asio::post(ioc, [&] {
        agrpc::run(grpc_context, ioc, [&] {
            return grpc_context.is_stopped();
        });
    });



    ioc.run();

    return EXIT_SUCCESS;
}
