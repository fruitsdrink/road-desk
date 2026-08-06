-- Mirror of internal/db/migrations/008_agent_lane_number.sql
ALTER TABLE agents
  ADD COLUMN IF NOT EXISTS lane_number TEXT NOT NULL DEFAULT '';
