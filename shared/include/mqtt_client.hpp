
#ifndef MQTTCLIENTHANDLER_HPP
#define MQTTCLIENTHANDLER_HPP

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/mqtt5/mqtt_client.hpp>
#include <boost/asio/co_spawn.hpp>
#include <unordered_set>
#include <boost/asio/strand.hpp>
#include <boost/asio/experimental/channel.hpp>

namespace asio = boost::asio;

struct MqttClientConfig
{
    std::string host = "0.0.0.0";
    uint port = 1883;
    std::string client_id = "fleet_manager";
};

struct MqttMessage {
    std::string topic;
    std::string payload;
};

using mqtt_channel_t = boost::asio::experimental::channel<void(boost::system::error_code, MqttMessage)>;

class MqttClientHandler {
public:
    MqttClientHandler(asio::io_context &ioc, const MqttClientConfig &config);
    ~MqttClientHandler();

    void start();
    void stop();
    asio::awaitable<std::optional<MqttMessage>> get_message_received();
    void subscribe_topic(const std::string& topic);
    void unsubscribe_topic(const std::string& topic);
    void publish_topic(const std::string& topic, const std::string& payload, int qos = 0, bool retained = false);

private:
    asio::awaitable<void> do_publish(std::string topic, std::string payload, int qos, bool retained);
    asio::awaitable<void> listen_loop();
    asio::awaitable<void> co_subscribe();

    boost::mqtt5::mqtt_client<asio::ip::tcp::socket> mqtt_client;
    asio::strand<asio::io_context::executor_type> network_strand;
    const MqttClientConfig& client_config;

    std::unordered_set<std::string> active_topics;
    std::mutex topics_mutex;

    mqtt_channel_t message_channel;
};



#endif //MQTTCLIENTHANDLER_HPP
