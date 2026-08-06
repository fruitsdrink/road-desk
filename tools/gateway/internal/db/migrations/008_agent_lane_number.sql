-- Agent lane number (optional, e.g. 01 / A1), set by admin.
ALTER TABLE agents
  ADD COLUMN IF NOT EXISTS lane_number TEXT NOT NULL DEFAULT '';
