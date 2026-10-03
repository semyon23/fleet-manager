
#include "../include/mqtt_client.hpp"
#include <boost/asio/as_tuple.hpp>
#include <print>
constexpr auto use_nothrow_awaitable = boost::asio::as_tuple(asio::deferred);

MqttClientHandler::MqttClientHandler(asio::io_context &ioc,  const MqttClientConfig &config)
    : mqtt_client(ioc),
    client_config(config),
    network_strand(asio::make_strand(ioc)),
    message_channel(ioc, 100){}

MqttClientHandler::~MqttClientHandler(){
    stop();
}

void MqttClientHandler::start(){

    boost::mqtt5::will last_will( "fms_client",
                    "online",
                    boost::mqtt5::qos_e::at_least_once,
                    boost::mqtt5::retain_e::no);

    mqtt_client.brokers(client_config.host, client_config.port)
        .will(std::move(last_will))
        .credentials(client_config.client_id)
        .async_run(boost::asio::detached);

    asio::co_spawn(network_strand, listen_loop(), boost::asio::detached);

    std::println("[MQTT] Client started, connection to the broker {}:{}", client_config.host, client_config.port);
}

void MqttClientHandler::stop(){
    message_channel.cancel();
    std::cout << "[MQTT] Message channel cancel" << std::endl;

    mqtt_client.cancel();
    std::cout << "[MQTT] Stopped " << std::endl;
}

asio::awaitable<void> MqttClientHandler::co_subscribe(){
    std::vector<std::string> topics_to_restore;
    {
        std::lock_guard<std::mutex> lock(topics_mutex);
        topics_to_restore.assign(active_topics.begin(), active_topics.end());
    }

    if (topics_to_restore.empty()) {
        co_return;
    }

    std::vector<boost::mqtt5::subscribe_topic> mqtt_topics;
    mqtt_topics.reserve(topics_to_restore.size());


    boost::mqtt5::subscribe_options options;
    options.max_qos = boost::mqtt5::qos_e::at_least_once;
    options.no_local = boost::mqtt5::no_local_e::no;
    options.retain_as_published = boost::mqtt5::retain_as_published_e::retain;
    options.retain_handling = boost::mqtt5::retain_handling_e::send;

    for (const auto& topic_str : topics_to_restore) {
        mqtt_topics.emplace_back(topic_str, options);
    }

    std::cout << "[MQTT] Bulk resubscribing to " << mqtt_topics.size() << " topics..." << std::endl;

    auto&& [ec, sub_codes, props] = co_await mqtt_client.async_subscribe(
        mqtt_topics,
        boost::mqtt5::subscribe_props{},
        use_nothrow_awaitable
    );

    if (ec) {
        std::cerr << "[MQTT] Bulk subscribe network error: " << ec.message() << std::endl;
    } else {
        std::cout << "[MQTT] Bulk subscribe completed successfully. Verifying reason codes..." << std::endl;

        for (size_t i = 0; i < sub_codes.size(); ++i) {
            if (sub_codes[i]) {
                std::cerr << "[MQTT] Broker rejected topic [" << topics_to_restore[i]
                          << "]. Reason: " << sub_codes[i].message() << std::endl;
            }
        }
    }
    co_return;
}

asio::awaitable<std::optional<MqttMessage>> MqttClientHandler::get_message_received(){
    auto [ec, msg] = co_await message_channel.async_receive(use_nothrow_awaitable);
    if (ec) co_return std::nullopt;

    co_return std::move(msg);
}


asio::awaitable<void> MqttClientHandler::listen_loop()
{
    while (true) {

        auto [ec, topic, payload, props] = co_await mqtt_client.async_receive(use_nothrow_awaitable);

        if (!ec)
        {
            if (!topic.empty()) {
                //std::cout << "[MQTT] Topic : "<< topic << " Payload : "<< payload <<std::endl;
                message_channel.try_send(boost::system::error_code{}, MqttMessage{std::move(topic), std::move(payload)});
            }
        }else if (ec == boost::mqtt5::client::error::session_expired){
            std::cerr << "[MQTT] Session expired on broker. Waiting before resubscribing..." << std::endl;

            asio::steady_timer timer(co_await asio::this_coro::executor);
            timer.expires_after(std::chrono::seconds(1));
            co_await timer.async_wait(use_nothrow_awaitable);

            std::cout << "[MQTT] Attempting to restore active_topics..." << std::endl;
            co_await co_subscribe();
            continue;
        }else {
            if (ec == asio::error::operation_aborted) {
                std::cout << "[MQTT] Listen loop stopped gracefully." << std::endl;
                break;
            }

            std::cerr << "[MQTT] Receive error: " << ec.message() << ". Throttling read..." << std::endl;

            asio::steady_timer timer(co_await asio::this_coro::executor);
            timer.expires_after(std::chrono::seconds(2));
            co_await timer.async_wait(use_nothrow_awaitable);
        }
    }
}

void MqttClientHandler::subscribe_topic(const std::string& topic){
    active_topics.insert(topic);
    boost::asio::co_spawn(network_strand,
        [this, topic]() -> asio::awaitable<void> {

            boost::mqtt5::subscribe_topic sub_topic = boost::mqtt5::subscribe_topic{
                topic,
                boost::mqtt5::subscribe_options{
                    boost::mqtt5::qos_e::at_least_once, // QoS 1
                    boost::mqtt5::no_local_e::no,
                    boost::mqtt5::retain_as_published_e::retain,
                    boost::mqtt5::retain_handling_e::send
                }
            };

            std::cout << "[MQTT] Sending SUBSCRIBE for topic: " << topic << std::endl;

            auto&& [ec, sub_codes, props] = co_await mqtt_client.async_subscribe(
                sub_topic, boost::mqtt5::subscribe_props{}, use_nothrow_awaitable
            );

            if (ec) {
                std::cerr << "[MQTT] Subscribe network error: " << ec.message() << std::endl;
            } else if (!sub_codes.empty() && sub_codes[0]) {
                std::cerr << "[MQTT] Broker rejected subscription. Reason: " << sub_codes[0].message() << std::endl;
            } else {
                std::cout << "[MQTT] Successfully subscribed to: " << topic << std::endl;
            }
            co_return;
        },
        boost::asio::detached
    );
}

void MqttClientHandler::unsubscribe_topic(const std::string& topic){
    active_topics.erase(topic);
    boost::asio::co_spawn(network_strand,
        [this, topic]() -> asio::awaitable<void> {
            std::cout << "[MQTT] Sending UNSUBSCRIBE for topic: " << topic << std::endl;

            auto&& [ec, unsub_codes, props] = co_await mqtt_client.async_unsubscribe(
                topic, boost::mqtt5::unsubscribe_props{}, use_nothrow_awaitable
            );

            if (ec) {
                std::cerr << "[MQTT] Unsubscribe network error: " << ec.message() << std::endl;
            } else if (!unsub_codes.empty() && unsub_codes[0]) {
                std::cerr << "[MQTT] Broker rejected unsubscribe: " << unsub_codes[0].message() << std::endl;
            } else {
                std::cout << "[MQTT] Successfully unsubscribed from: " << topic << std::endl;
            }
            co_return;
        },
        boost::asio::detached
    );
}

void MqttClientHandler::publish_topic(const std::string& topic_string, const std::string& payload_string, int qos, bool retained) {

    boost::asio::co_spawn(network_strand,
                          do_publish(topic_string, payload_string, qos, retained),
                          boost::asio::detached);
}

// QoS у async_publish — параметр шаблона, а от него зависит и результат операции:
//   QoS 0 -> (error_code)                          брокер ничего не подтверждает
//   QoS 1 -> (error_code, reason_code, puback_props)
//   QoS 2 -> (error_code, reason_code, pubcomp_props)
// Поэтому выбор QoS во время работы — через switch.
asio::awaitable<void> MqttClientHandler::do_publish(std::string topic, std::string payload,
                                                    int qos, bool retained) {
    using boost::mqtt5::qos_e;
    const auto retain = retained ? boost::mqtt5::retain_e::yes : boost::mqtt5::retain_e::no;

    boost::system::error_code ec;
    // Для QoS 0 брокер не отвечает — считаем успехом. reason_code{} по умолчанию — НЕ успех.
    boost::mqtt5::reason_code rc = boost::mqtt5::reason_codes::success;

    switch (qos) {
        case 0: {
            auto [e] = co_await mqtt_client.async_publish<qos_e::at_most_once>(
                topic, payload, retain, boost::mqtt5::publish_props{}, use_nothrow_awaitable);
            ec = e;
            break;
        }
        case 1: {
            auto [e, r, props] = co_await mqtt_client.async_publish<qos_e::at_least_once>(
                topic, payload, retain, boost::mqtt5::publish_props{}, use_nothrow_awaitable);
            ec = e;
            rc = r;
            break;
        }
        case 2: {
            auto [e, r, props] = co_await mqtt_client.async_publish<qos_e::exactly_once>(
                topic, payload, retain, boost::mqtt5::publish_props{}, use_nothrow_awaitable);
            ec = e;
            rc = r;
            break;
        }
        default:
            std::cerr << "[MQTT] Invalid QoS " << qos << " for topic [" << topic << "]" << std::endl;
            co_return;
    }

    if (ec) {
        std::cerr << "[MQTT] Publish network error on topic [" << topic << "]: " << ec.message() << std::endl;
    } else if (rc) {
        std::cerr << "[MQTT] Broker rejected publish on topic [" << topic << "]. Reason: " << rc.message()
                  << std::endl;
    }
}

// void MqttClientHandler::publish_topic(const std::string& topic_string, const std::string& payload_string, int qos, bool retained) {
//     boost::asio::co_spawn(network_strand,
//         [this, topic_string, payload_string, qos, retained]() -> asio::awaitable<void> {
//             auto&& [ec, rc, props] = co_await mqtt_client.async_publish<boost::mqtt5::qos_e::at_least_once>(
//                 topic_string,
//                 payload_string,
//                 boost::mqtt5::retain_e::no,
//                 boost::mqtt5::publish_props{},
//                 use_nothrow_awaitable
//             );
//
//             if (ec) {
//                 std::cerr << "[MQTT] Publish network error on topic [" << topic_string
//                           << "]: " << ec.message() << std::endl;
//             } else if (rc) {
//                 std::cerr << "[MQTT] Broker rejected publish on topic [" << topic_string
//                           << "]. Reason: " << rc.message() << std::endl;
//             }
//             // else {
//             //     std::cout << "[MQTT] Successfully published message to: " << topic_string << std::endl;
//             // }
//
//             co_return;
//         },
//         boost::asio::detached
//     );
// }