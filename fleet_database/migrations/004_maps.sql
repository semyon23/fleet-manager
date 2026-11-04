-- Карты: пишет fleet_control (ControlStorage), UI читает через REST.
-- Карт может быть несколько, все действуют одновременно; map_id — VDA mapId роботов.
-- body — документ редактора UI (JSONB) без полей строки; image — картинка отдельно:
-- в списке карт и в графе для control она не нужна.
CREATE TABLE maps (
                      map_id     TEXT PRIMARY KEY,
                      name       TEXT NOT NULL,
                      version    INTEGER NOT NULL DEFAULT 1,
                      body       JSONB NOT NULL,
                      image      TEXT NOT NULL DEFAULT '',
                      created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
                      updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);