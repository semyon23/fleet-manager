CREATE TABLE dispatch_execution_heads (
                                          mission_id text PRIMARY KEY,
                                          robot_id text NOT NULL DEFAULT '',
                                          version bigint NOT NULL DEFAULT 0 CHECK (version >= 0),
                                          terminal boolean NOT NULL DEFAULT false,
                                          uncertain boolean NOT NULL DEFAULT true,
                                          execution bytea,
                                          grant_step text,
                                          grant_order text,
                                          grant_update bigint,
                                          FOREIGN KEY (mission_id, grant_step, grant_order, grant_update)
                                              REFERENCES dispatch_execution_orders(mission_id, step_id, order_id, update_id)
);

CREATE INDEX dispatch_execution_open ON dispatch_execution_heads(mission_id)
    WHERE NOT terminal OR uncertain;

INSERT INTO dispatch_execution_heads(mission_id)
SELECT mission_id FROM dispatch_add_receipts;

INSERT INTO dispatch_execution_heads(mission_id, robot_id)
SELECT mission_id,
       CASE WHEN min(robot_id) = max(robot_id) THEN min(robot_id) ELSE '' END
FROM dispatch_execution_orders
GROUP BY mission_id
    ON CONFLICT (mission_id) DO UPDATE SET robot_id = EXCLUDED.robot_id;

CREATE FUNCTION dispatch_claim_head() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
INSERT INTO dispatch_execution_heads(mission_id) VALUES (NEW.mission_id)
    ON CONFLICT DO NOTHING;
RETURN NEW;
END $$;

CREATE TRIGGER dispatch_claim_head AFTER INSERT ON dispatch_add_receipts
    FOR EACH ROW EXECUTE FUNCTION dispatch_claim_head();