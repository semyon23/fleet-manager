#ifndef FLEETMANAGERSYSTEM_ALARMENGINE_HPP
#define FLEETMANAGERSYSTEM_ALARMENGINE_HPP

#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "alarms/AlarmRules.hpp"
#include "services/Services.hpp"

// Тревоги (M-5, контракт 8): что требует человека. Правила — AlarmRules; здесь — их жизнь:
// условие появилось → (через задержку) RAISED, ушло → CLEARED, оператор → ACKED. Одна активная на key.
// В памяти — открытые (активные или не подтверждённые); запись в базу — через очередь, с событиями журнала.
//
// Подавление: нет связи с dispatch — статусы роботов устарели, их тревоги не трогаем, горит одна SERVICE_DOWN.
class AlarmEngine {
public:
    AlarmEngine(asio::io_context& io_context, Services& services, double low_battery_percent);

    void start();   // загрузить открытые тревоги из базы, таймер задержек
    void stop();
    bool loaded() const { return loaded_; }

    void on_robot(const std::string& robot_id, const fms::v1::RobotStatus* status);   // свежий статус робота
    void remove_robot(const std::string& robot_id);   // удалён: его тревоги снять и подтвердить
    void keep_only(const std::set<std::string>& robot_ids);   // остальных роботов в fms уже нет
    void set_dispatch_connected(bool connected);

    // Подтвердить. nullopt — открытой тревоги с таким id нет (неизвестна или уже закрыта)
    std::optional<fms::database::v1::AlarmRecord> ack(const std::string& alarm_id, const std::string& by);

private:
    struct Waiting {             // условие есть, задержка ещё не прошла
        AlarmCondition condition;
        std::string robot_id;    // "" — системная
        int64_t since_ms = 0;
    };
    struct Pending {             // ещё не записано: последняя версия тревоги и её события
        fms::database::v1::AlarmRecord record;
        std::vector<fms::database::v1::Event> events;
    };

    void evaluate(const std::string& robot_id);
    void want(const AlarmCondition& condition, const std::string& robot_id);   // условие есть сейчас
    void raise(const AlarmCondition& condition, const std::string& robot_id);
    void clear(const std::string& alarm_id, const std::string& why);
    void acknowledge(fms::database::v1::AlarmRecord& alarm, const std::string& by);
    void save(const fms::database::v1::AlarmRecord& alarm, const std::string& event_type, const std::string& why = "");
    asio::awaitable<void> load();
    asio::awaitable<void> tick_loop();
    asio::awaitable<void> write_loop();

    using Signal = asio::experimental::channel<void(boost::system::error_code)>;

    asio::io_context& io_context_;
    Services& services_;
    double low_battery_percent_;
    bool loaded_ = false;
    bool dispatch_connected_ = true;   // пока FleetSync не сказал обратного
    std::map<std::string, std::optional<fms::v1::RobotStatus>> robots_;   // последний статус каждого робота
    std::optional<std::set<std::string>> known_robots_;
    std::map<std::string, fms::database::v1::AlarmRecord> open_;        // alarm_id → открытая
    std::map<std::string, std::string> active_by_key_;                  // key → alarm_id активной
    std::map<std::string, Waiting> waiting_;                            // key → ждёт задержки
    std::map<std::string, Pending> pending_;                            // alarm_id → к записи
    Signal wakeup_;
    asio::steady_timer tick_timer_;
    asio::steady_timer retry_timer_;
    bool stopped_ = false;
};

#endif //FLEETMANAGERSYSTEM_ALARMENGINE_HPP