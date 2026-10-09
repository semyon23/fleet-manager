#ifndef STATEPUBLISHER_HPP
#define STATEPUBLISHER_HPP
#include "../../vda5050/vda5050.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <print>
#include "vda_topics.hpp"
namespace asio = boost::asio;
namespace vda = vda5050;

struct PublishMessage{
    std::string topic;
    std::string payload;
    int qos = 0;
    bool retained = false;
};

using publishing_channel = asio::experimental::channel<void(boost::system::error_code, PublishMessage)>;
using ptrPublishingChannel= std::shared_ptr<publishing_channel>;

class StatePublisher {
public:
    StatePublisher(asio::any_io_executor ex, ptrPublishingChannel channel, vda::State& state, VdaTopics topics, PublisherConfig config);
    void set_factsheet(vda5050::Factsheet factsheet);
    void stop();
    void on_link_up();
    void emit_state_changed();
    void publish_factsheet();
    asio::awaitable<void> run_visu_publisher_loop();
    asio::awaitable<void> run_state_publisher_loop();
private:
    void pub_state_topic();
    void pub_factsheet_topic();
    void pub_visu_topic();
    void pub_connection_topic();
    vda::Header make_header(std::uint32_t& counter) const;

    vda::State& State_;
    vda5050::State Last_sent_state;
    vda::Visualization Visu_;
    vda::Connection Connection_;
    vda::Factsheet Factsheet_;

    ptrPublishingChannel channel_;
    VdaTopics topics_;
    PublisherConfig config_;
    asio::experimental::channel<void(boost::system::error_code, bool)> state_trigger;
    asio::steady_timer visu_timer;
    asio::steady_timer state_timer;
    bool state_sent_once{false};
    bool stopping_{false};
};



#endif //STATEPUBLISHER_HPP
