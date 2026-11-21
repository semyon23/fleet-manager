-- Разные подтверждённые попытки назначения одной миссии должны иметь
-- независимые Add/Extend/Cancel-команды.
-- Старые записи относятся к попытке 0.

ALTER TABLE control_commands
    ADD COLUMN attempt BIGINT NOT NULL DEFAULT 0
        CHECK (attempt BETWEEN 0 AND 4294967295);

ALTER TABLE control_commands
DROP CONSTRAINT
        control_commands_mission_id_kind_move_node_id_revision_key;

ALTER TABLE control_commands
    ADD CONSTRAINT control_commands_logical_command
        UNIQUE (
                mission_id,
                attempt,
                kind,
                move_node_id,
                revision
            );