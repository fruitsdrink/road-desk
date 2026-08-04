-- Mirror of internal/db/migrations/002_departments_users.sql (embedded at runtime).

CREATE TABLE IF NOT EXISTS departments (
  id         BIGSERIAL PRIMARY KEY,
  name       TEXT NOT NULL UNIQUE,
  sort_order INT NOT NULL DEFAULT 0,
  is_system  BOOLEAN NOT NULL DEFAULT FALSE,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS users (
  id              BIGSERIAL PRIMARY KEY,
  username        TEXT NOT NULL UNIQUE,
  password_hash   TEXT NOT NULL,
  role            TEXT NOT NULL CHECK (role IN ('admin', 'viewer')),
  department_id   BIGINT NOT NULL REFERENCES departments(id),
  enabled         BOOLEAN NOT NULL DEFAULT TRUE,
  created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  updated_at      TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS users_department_id_idx ON users (department_id);

INSERT INTO departments (name, sort_order, is_system)
SELECT '系统管理', 0, TRUE
WHERE NOT EXISTS (SELECT 1 FROM departments WHERE name = '系统管理');

INSERT INTO users (username, password_hash, role, department_id, enabled, created_at)
SELECT a.username, a.password_hash, 'admin', d.id, TRUE, a.created_at
FROM admins a
CROSS JOIN LATERAL (
  SELECT id FROM departments WHERE name = '系统管理' LIMIT 1
) d
WHERE NOT EXISTS (SELECT 1 FROM users u WHERE u.username = a.username);
