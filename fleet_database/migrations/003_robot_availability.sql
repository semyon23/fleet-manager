-- Готовность робота к работе (availability): считает и пишет fleet_control, одна строка на робота.
-- RobotAvailability из fleet_database.proto в JSON (protobuf) — как robot_status.
CREATE TABLE robot_availability (
                                    robot_id     TEXT PRIMARY KEY REFERENCES robots (robot_id) ON DELETE CASCADE,
                                    availability JSONB NOT NULL,
                                    saved_at     TIMESTAMPTZ NOT NULL DEFAULT now()
);