-- Роботы: пишет fleet_control через RobotRegistry
CREATE TABLE robots (
                        robot_id      TEXT PRIMARY KEY,
                        manufacturer  TEXT NOT NULL,
                        serial_number TEXT NOT NULL,
                        name          TEXT NOT NULL DEFAULT '',
                        labels        JSONB NOT NULL DEFAULT '{}',
                        lifecycle     TEXT NOT NULL DEFAULT 'ACTIVE' CHECK (lifecycle IN ('ACTIVE', 'PENDING_DELETE')),
                        created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
                        updated_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
                        UNIQUE (manufacturer, serial_number)   -- один физический робот — одна запись
);