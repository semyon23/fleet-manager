ALTER TABLE missions ADD COLUMN dispatch_final_version bigint
    CHECK (dispatch_final_version IS NULL OR
           (dispatch_final_version > 0 AND state IN ('COMPLETED', 'CANCELED', 'FAILED')));

CREATE INDEX control_commands_open_missions ON control_commands(mission_id, attempt) WHERE NOT resolved;
