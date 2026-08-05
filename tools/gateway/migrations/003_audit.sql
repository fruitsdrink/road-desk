-- Mirror of internal/db/migrations/003_audit.sql (embedded at runtime).

CREATE TABLE IF NOT EXISTS audit_sessions (
  id                  UUID PRIMARY KEY,
  operator_user_id    BIGINT NULL REFERENCES users(id) ON DELETE SET NULL,
  operator_name       TEXT NOT NULL DEFAULT '',
  viewer_host         TEXT NOT NULL DEFAULT '',
  viewer_ip           TEXT NOT NULL DEFAULT '',
  agent_id            TEXT NOT NULL DEFAULT '',
  agent_name          TEXT NOT NULL DEFAULT '',
  agent_endpoint      TEXT NOT NULL DEFAULT '',
  mode                TEXT NOT NULL DEFAULT 'unknown'
                      CHECK (mode IN ('control', 'view_only', 'unknown')),
  result              TEXT NOT NULL DEFAULT 'ok'
                      CHECK (result IN (
                        'ok', 'auth_fail', 'capacity_reject', 'connect_fail',
                        'tls_fail', 'cancelled', 'unknown'
                      )),
  disconnect_reason   TEXT NOT NULL DEFAULT '',
  used_clipboard      BOOLEAN NOT NULL DEFAULT FALSE,
  used_file_transfer  BOOLEAN NOT NULL DEFAULT FALSE,
  attempted_at        TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  opened_at           TIMESTAMPTZ NULL,
  closed_at           TIMESTAMPTZ NULL,
  partial             BOOLEAN NOT NULL DEFAULT TRUE,
  meta                JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS audit_sessions_attempted_at_idx
  ON audit_sessions (attempted_at DESC);
CREATE INDEX IF NOT EXISTS audit_sessions_agent_attempted_idx
  ON audit_sessions (agent_id, attempted_at DESC);
CREATE INDEX IF NOT EXISTS audit_sessions_operator_attempted_idx
  ON audit_sessions (operator_user_id, attempted_at DESC);
CREATE INDEX IF NOT EXISTS audit_sessions_result_idx
  ON audit_sessions (result);
