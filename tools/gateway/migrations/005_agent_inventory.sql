-- Mirror of internal/db/migrations/005_agent_inventory.sql
ALTER TABLE agents
  ADD COLUMN IF NOT EXISTS inventory JSONB NOT NULL DEFAULT '{}'::jsonb;
