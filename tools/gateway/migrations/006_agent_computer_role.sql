-- Mirror of internal/db/migrations/006_agent_computer_role.sql
ALTER TABLE agents
  ADD COLUMN IF NOT EXISTS computer_role TEXT NOT NULL DEFAULT '';
