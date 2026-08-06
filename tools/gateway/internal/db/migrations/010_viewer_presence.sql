-- Viewer (操作端) presence for admin online list. No groups.
CREATE TABLE IF NOT EXISTS viewer_presence (
  viewer_id    TEXT PRIMARY KEY,
  hostname     TEXT NOT NULL DEFAULT '',
  username     TEXT NOT NULL DEFAULT '',
  auth_mode    TEXT NOT NULL DEFAULT '',
  version      TEXT NOT NULL DEFAULT '',
  client_ip    TEXT NOT NULL DEFAULT '',
  last_seen_at TIMESTAMPTZ,
  created_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  updated_at   TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS viewer_presence_last_seen_idx
  ON viewer_presence (last_seen_at DESC NULLS LAST);
