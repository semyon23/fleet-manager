CREATE TABLE dispatch_add_receipts (
                                       mission_id    TEXT PRIMARY KEY,
                                       attempt       BIGINT NOT NULL CHECK (attempt BETWEEN 0 AND 4294967295),
                                       request_bytes BYTEA NOT NULL,
                                       reply_bytes   BYTEA,
                                       created_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);