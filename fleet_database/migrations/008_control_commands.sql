-- Намерения control.
-- Запись должна завершиться до отправки команды dispatch.
--
-- Нет FK на робота/карту/миссию:
-- удаление справочника не должно уничтожать сведения
-- о потенциально выданном разрешении.

CREATE TABLE control_commands (
                                  sequence_id BIGSERIAL PRIMARY KEY,

                                  command_id TEXT NOT NULL UNIQUE,
                                  robot_id TEXT NOT NULL,
                                  mission_id TEXT NOT NULL,
                                  map_id TEXT NOT NULL,

                                  map_version BIGINT NOT NULL
                                      CHECK (map_version BETWEEN 1 AND 4294967295),

                                  kind SMALLINT NOT NULL CHECK (kind IN (1, 2, 3)),
                                  move_node_id TEXT NOT NULL,

                                  revision BIGINT NOT NULL
                                      CHECK (revision BETWEEN 0 AND 4294967295),

                                  payload BYTEA NOT NULL,

                                  resolved BOOLEAN NOT NULL DEFAULT FALSE,
                                  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
                                  resolved_at TIMESTAMPTZ,

                                  UNIQUE (mission_id, kind, move_node_id, revision),

                                  CHECK (
                                      (kind = 2 AND move_node_id <> '' AND revision > 0)
                                          OR
                                      (kind IN (1, 3) AND move_node_id = '' AND revision = 0)
                                      )
);

CREATE INDEX control_commands_pending
    ON control_commands (sequence_id)
    WHERE NOT resolved;

CREATE INDEX control_commands_by_robot
    ON control_commands (robot_id, sequence_id);