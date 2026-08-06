-- Admin-managed device role catalog (agents.computer_role stores the role name).
CREATE TABLE IF NOT EXISTS computer_roles (
  id         BIGSERIAL PRIMARY KEY,
  name       TEXT NOT NULL UNIQUE,
  sort_order INT NOT NULL DEFAULT 0,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

INSERT INTO computer_roles (name, sort_order) VALUES
  ('收费', 0),
  ('发卡', 1),
  ('机器人', 2),
  ('维护工作站', 3),
  ('前台', 4),
  ('机房', 5)
ON CONFLICT (name) DO NOTHING;

-- Preserve any roles already assigned on agents that are not in the seed set.
INSERT INTO computer_roles (name, sort_order)
SELECT DISTINCT a.computer_role, 100
FROM agents a
WHERE a.computer_role <> ''
ON CONFLICT (name) DO NOTHING;
