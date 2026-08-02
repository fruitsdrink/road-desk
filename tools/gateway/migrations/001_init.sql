-- Mirror of internal/db/migrations/001_init.sql (embedded at runtime).
-- Kept here for operators reading the schema beside docker-compose.

CREATE TABLE IF NOT EXISTS admins (
  id            BIGSERIAL PRIMARY KEY,
  username      TEXT NOT NULL UNIQUE,
  password_hash TEXT NOT NULL,
  created_at    TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS secrets (
  name       TEXT PRIMARY KEY,
  value      TEXT NOT NULL,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS groups (
  id         BIGSERIAL PRIMARY KEY,
  parent_id  BIGINT REFERENCES groups(id) ON DELETE CASCADE,
  name       TEXT NOT NULL,
  sort_order INT NOT NULL DEFAULT 0,
  is_system  BOOLEAN NOT NULL DEFAULT FALSE,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE UNIQUE INDEX IF NOT EXISTS groups_root_name_uidx
  ON groups (name) WHERE parent_id IS NULL;

CREATE TABLE IF NOT EXISTS tags (
  id         BIGSERIAL PRIMARY KEY,
  name       TEXT NOT NULL UNIQUE,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS agents (
  agent_id         TEXT PRIMARY KEY,
  display_name     TEXT NOT NULL DEFAULT '',
  hostname         TEXT NOT NULL DEFAULT '',
  group_id         BIGINT NOT NULL REFERENCES groups(id),
  media_port       INT NOT NULL DEFAULT 38471,
  version          TEXT NOT NULL DEFAULT '',
  preferred_ipv4   TEXT NOT NULL DEFAULT '',
  last_seen_at     TIMESTAMPTZ,
  created_at       TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  updated_at       TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS agent_ipv4s (
  agent_id TEXT NOT NULL REFERENCES agents(agent_id) ON DELETE CASCADE,
  ipv4     TEXT NOT NULL,
  PRIMARY KEY (agent_id, ipv4)
);

CREATE TABLE IF NOT EXISTS agent_tags (
  agent_id TEXT NOT NULL REFERENCES agents(agent_id) ON DELETE CASCADE,
  tag_id   BIGINT NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
  PRIMARY KEY (agent_id, tag_id)
);

INSERT INTO groups (parent_id, name, sort_order, is_system)
SELECT NULL, '未分类', 0, TRUE
WHERE NOT EXISTS (SELECT 1 FROM groups WHERE name = '未分类' AND parent_id IS NULL);
