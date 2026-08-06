-- Mirror of internal/db/migrations/007_agent_install_location.sql
ALTER TABLE agents
  ADD COLUMN IF NOT EXISTS install_location TEXT NOT NULL DEFAULT '';
