CREATE TABLE dispatch_execution_orders (
                                           mission_id text NOT NULL,
                                           step_id text NOT NULL,
                                           order_id text NOT NULL,
                                           update_id bigint NOT NULL CHECK (update_id BETWEEN 0 AND 4294967295),
                                           robot_id text NOT NULL,
                                           released_nodes bigint NOT NULL,
                                           execution bytea NOT NULL,
                                           order_json jsonb NOT NULL,
                                           PRIMARY KEY (mission_id, step_id, order_id, update_id)
);