-- Agent install location (e.g. 入口01车道), set by admin.
ALTER TABLE agents
  ADD COLUMN IF NOT EXISTS install_location TEXT NOT NULL DEFAULT '';
