-- A5 / directory: Host machine inventory (OS, CPU, RAM, disks) from heartbeat.
ALTER TABLE agents
  ADD COLUMN IF NOT EXISTS inventory JSONB NOT NULL DEFAULT '{}'::jsonb;
