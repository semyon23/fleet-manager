-- События миссий (c3): история для UI — карточка миссии, вкладка History. Пишет fleet_control
-- вместе с миссией (SaveMission, одна транзакция), только дописываются.
-- details — подробности события (JSON); база их не разбирает, UI читает как есть.
CREATE TABLE mission_events (
                                id          BIGSERIAL PRIMARY KEY,      -- порядок событий
                                mission_id  TEXT NOT NULL REFERENCES missions (mission_id) ON DELETE CASCADE,
                                at          TIMESTAMPTZ NOT NULL,
                                robot_id    TEXT,                       -- NULL — миссия ещё не назначена
                                type        TEXT NOT NULL CHECK (type IN ('STATE', 'STEP', 'ACTION')),
                                details     JSONB NOT NULL
);

-- GET /missions/{id}/events: события одной миссии по порядку
CREATE INDEX mission_events_by_mission ON mission_events (mission_id, id);

-- Фильтр History ?from=&to= — по времени создания миссии
-- (missions_by_created (created_at DESC, mission_id DESC) из 005 подходит, новый индекс не нужен)