-- Audit event timeline (per-session). See docs/audit-implementation-plan.md §4.2.
-- Designed so Host can later append process_open / process_close without schema change.

CREATE TABLE IF NOT EXISTS audit_events (
  id          BIGSERIAL PRIMARY KEY,
  session_id  UUID NOT NULL REFERENCES audit_sessions(id) ON DELETE CASCADE,
  at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  source      TEXT NOT NULL
              CHECK (source IN ('viewer', 'agent', 'gateway')),
  -- Documented vocabulary (extensible): attempt|opened|closed|failed|flag|
  -- file_transfer|process_open|process_close|…
  type        TEXT NOT NULL,
  detail      JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  CONSTRAINT audit_events_type_len CHECK (char_length(type) BETWEEN 1 AND 64)
);

CREATE INDEX IF NOT EXISTS audit_events_session_at_idx
  ON audit_events (session_id, at ASC, id ASC);
-- Cross-session process / type queries (future process audit).
CREATE INDEX IF NOT EXISTS audit_events_type_at_idx
  ON audit_events (type, at DESC);
CREATE INDEX IF NOT EXISTS audit_events_at_idx
  ON audit_events (at DESC);
