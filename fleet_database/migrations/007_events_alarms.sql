-- Журнал (M-5): одна таблица событий вместо mission_events — миссии, роботы, тревоги, система.
-- Пишет fleet_control, только дописываются. details — подробности по type (JSON), база их не разбирает.
CREATE TABLE events (
                        id          BIGSERIAL PRIMARY KEY,      -- порядок событий
                        at          TIMESTAMPTZ NOT NULL,
                        source      TEXT NOT NULL CHECK (source IN ('MISSION', 'ROBOT', 'ALARM', 'SYSTEM')),
                        type        TEXT NOT NULL CHECK (type <> ''),
                        mission_id  TEXT REFERENCES missions (mission_id) ON DELETE CASCADE,
                        robot_id    TEXT,
                        alarm_id    TEXT,
                        details     JSONB NOT NULL
);

-- События миссий из 006 — с теми же id: порядок сохраняется
INSERT INTO events (id, at, source, type, mission_id, robot_id, details)
SELECT id, at, 'MISSION', type, mission_id, robot_id, details FROM mission_events ORDER BY id;
-- Счётчик id — дальше перенесённых
SELECT setval(pg_get_serial_sequence('events', 'id'), COALESCE(max(id), 0) + 1, false) FROM events;
DROP TABLE mission_events;

-- Карточка миссии, журнал робота, история тревоги, журнал по источнику (новые сверху), период
CREATE INDEX events_by_mission ON events (mission_id, id) WHERE mission_id IS NOT NULL;
CREATE INDEX events_by_robot ON events (robot_id, id) WHERE robot_id IS NOT NULL;
CREATE INDEX events_by_alarm ON events (alarm_id, id) WHERE alarm_id IS NOT NULL;
CREATE INDEX events_by_source ON events (source, id);
CREATE INDEX events_by_at ON events (at);

-- Тревоги (контракт 8.1): только то, что требует человека. Пишет fleet_control.
-- Без внешних ключей на роботов и миссии: история тревог остаётся после их удаления.
CREATE TABLE alarms (
                        alarm_id         TEXT PRIMARY KEY,
                        code             TEXT NOT NULL,
                        severity         TEXT NOT NULL CHECK (severity IN ('INFO', 'WARNING', 'ERROR', 'CRITICAL')),
                        source           TEXT NOT NULL CHECK (source IN ('ROBOT', 'MISSION', 'TRAFFIC', 'SYSTEM')),
                        robot_id         TEXT,
                        mission_id       TEXT,
                        title            TEXT NOT NULL,
                        detail           TEXT NOT NULL,
                        key              TEXT NOT NULL,
                        active           BOOLEAN NOT NULL,
                        created_at       TIMESTAMPTZ NOT NULL,
                        cleared_at       TIMESTAMPTZ,
                        acknowledged_at  TIMESTAMPTZ,
                        acknowledged_by  TEXT
);

-- Одна активная тревога на причину: тот же робот с той же ошибкой — одна строка, а не лавина
CREATE UNIQUE INDEX alarms_one_active_per_key ON alarms (key) WHERE active;
-- Список UI: новые сверху, страницы по курсору (created_at, alarm_id)
CREATE INDEX alarms_by_created ON alarms (created_at DESC, alarm_id DESC);
-- Открытые (активные или не подтверждённые): старт control и вкладка Alarms. Их мало — индекс маленький
CREATE INDEX alarms_open ON alarms (created_at) WHERE active OR acknowledged_at IS NULL;
CREATE INDEX alarms_by_robot ON alarms (robot_id, created_at DESC);