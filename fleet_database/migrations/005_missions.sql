-- Миссии fms: пишет fleet_control (ControlStorage), UI читает через REST (контракт, раздел 7).
-- body — объект Mission контракта целиком; остальные колонки — копии его полей для фильтров
-- и сортировки, их держит control.
-- Без внешних ключей на robots и maps: история миссий остаётся после удаления робота или карты.
CREATE TABLE missions (
                          mission_id  TEXT PRIMARY KEY,
                          map_id      TEXT NOT NULL,
                          robot_id    TEXT,                       -- NULL — ещё не назначен
                          state       TEXT NOT NULL CHECK (state IN ('QUEUED', 'ASSIGNED', 'RUNNING', 'PAUSED',
                                                                     'CANCELING', 'COMPLETED', 'FAILED', 'CANCELED')),
                          priority    SMALLINT NOT NULL CHECK (priority BETWEEN 0 AND 3),
                          body        JSONB NOT NULL,
                          created_at  TIMESTAMPTZ NOT NULL,
                          updated_at  TIMESTAMPTZ NOT NULL,
                          finished_at TIMESTAMPTZ
);

-- Список UI: новые сверху, страницы по курсору (created_at, mission_id)
CREATE INDEX missions_by_created ON missions (created_at DESC, mission_id DESC);
-- Фильтры ?robotId= и ?mapId=
CREATE INDEX missions_by_robot ON missions (robot_id, created_at DESC);
CREATE INDEX missions_by_map ON missions (map_id, created_at DESC);
-- Незавершённые: старт control и ?state=QUEUED,ASSIGNED,RUNNING,PAUSED. Их мало, индекс маленький
CREATE INDEX missions_active ON missions (created_at)
    WHERE state IN ('QUEUED', 'ASSIGNED', 'RUNNING', 'PAUSED', 'CANCELING');