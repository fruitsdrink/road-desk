-- Agent computer role (收费 / 发卡 / 机器人 / 维护工作站 / …), set by admin.
ALTER TABLE agents
  ADD COLUMN IF NOT EXISTS computer_role TEXT NOT NULL DEFAULT '';
