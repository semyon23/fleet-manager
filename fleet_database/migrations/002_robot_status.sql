-- Последний статус робота от dispatch: fms.v1.RobotStatus в JSON (protobuf), без истории.
-- JSON, а не столбцы: новое поле в proto не требует миграции.
CREATE TABLE robot_status (
                              robot_id TEXT PRIMARY KEY REFERENCES robots (robot_id) ON DELETE CASCADE,
                              status   JSONB NOT NULL,
                              saved_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Последний factsheet робота: VDA JSON, как пришёл от робота
CREATE TABLE robot_factsheets (
                                  robot_id    TEXT PRIMARY KEY REFERENCES robots (robot_id) ON DELETE CASCADE,
                                  factsheet   JSONB NOT NULL,
                                  received_at TIMESTAMPTZ NOT NULL
);