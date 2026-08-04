package store

import (
	"context"
	"errors"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

type Store struct {
	Pool         *pgxpool.Pool
	OnlineAfterS int
}

type Group struct {
	ID        int64   `json:"id"`
	ParentID  *int64  `json:"parentId"`
	Name      string  `json:"name"`
	SortOrder int     `json:"sortOrder"`
	IsSystem  bool    `json:"isSystem"`
}

type Tag struct {
	ID         int64  `json:"id"`
	Name       string `json:"name"`
	AgentCount int64  `json:"agentCount"`
}

type Agent struct {
	AgentID       string     `json:"agentId"`
	DisplayName   string     `json:"displayName"`
	Hostname      string     `json:"hostname"`
	GroupID       int64      `json:"groupId"`
	MediaPort     int        `json:"mediaPort"`
	Version       string     `json:"version"`
	PreferredIPv4 string     `json:"preferredIpv4"`
	IPv4s         []string   `json:"ipv4s"`
	TagIDs        []int64    `json:"tagIds"`
	TagNames      []string   `json:"tagNames,omitempty"`
	LastSeenAt    *time.Time `json:"lastSeenAt"`
	Online        bool       `json:"online"`
	CreatedAt     time.Time  `json:"createdAt"`
	UpdatedAt     time.Time  `json:"updatedAt"`
}

func (s *Store) UncategorizedGroupID(ctx context.Context) (int64, error) {
	var id int64
	err := s.Pool.QueryRow(ctx, `
		SELECT id FROM groups WHERE name='未分类' AND parent_id IS NULL LIMIT 1`).Scan(&id)
	return id, err
}

func (s *Store) ListGroups(ctx context.Context) ([]Group, error) {
	rows, err := s.Pool.Query(ctx, `
		SELECT id, parent_id, name, sort_order, is_system FROM groups
		ORDER BY sort_order, id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Group
	for rows.Next() {
		var g Group
		if err := rows.Scan(&g.ID, &g.ParentID, &g.Name, &g.SortOrder, &g.IsSystem); err != nil {
			return nil, err
		}
		out = append(out, g)
	}
	return out, rows.Err()
}

func (s *Store) CreateGroup(ctx context.Context, parentID *int64, name string, sortOrder int) (Group, error) {
	var g Group
	err := s.Pool.QueryRow(ctx, `
		INSERT INTO groups(parent_id, name, sort_order) VALUES ($1,$2,$3)
		RETURNING id, parent_id, name, sort_order, is_system`, parentID, name, sortOrder).
		Scan(&g.ID, &g.ParentID, &g.Name, &g.SortOrder, &g.IsSystem)
	return g, err
}

func (s *Store) UpdateGroup(ctx context.Context, id int64, parentID *int64, name *string, sortOrder *int) (Group, error) {
	var g Group
	err := s.Pool.QueryRow(ctx, `
		UPDATE groups SET
			parent_id = COALESCE($2, parent_id),
			name = COALESCE($3, name),
			sort_order = COALESCE($4, sort_order)
		WHERE id=$1
		RETURNING id, parent_id, name, sort_order, is_system`, id, parentID, name, sortOrder).
		Scan(&g.ID, &g.ParentID, &g.Name, &g.SortOrder, &g.IsSystem)
	return g, err
}

func (s *Store) DeleteGroup(ctx context.Context, id int64) error {
	var isSystem bool
	var n int
	err := s.Pool.QueryRow(ctx, `SELECT is_system FROM groups WHERE id=$1`, id).Scan(&isSystem)
	if err != nil {
		return err
	}
	if isSystem {
		return errors.New("不能删除系统分组")
	}
	if err := s.Pool.QueryRow(ctx, `SELECT COUNT(*) FROM groups WHERE parent_id=$1`, id).Scan(&n); err != nil {
		return err
	}
	if n > 0 {
		return errors.New("分组下仍有子分组")
	}
	if err := s.Pool.QueryRow(ctx, `SELECT COUNT(*) FROM agents WHERE group_id=$1`, id).Scan(&n); err != nil {
		return err
	}
	if n > 0 {
		return errors.New("分组下仍有 Agent")
	}
	_, err = s.Pool.Exec(ctx, `DELETE FROM groups WHERE id=$1`, id)
	return err
}

func (s *Store) ListTags(ctx context.Context) ([]Tag, error) {
	rows, err := s.Pool.Query(ctx, `
		SELECT t.id, t.name, COUNT(at.agent_id)::bigint
		FROM tags t
		LEFT JOIN agent_tags at ON at.tag_id = t.id
		GROUP BY t.id, t.name
		ORDER BY t.name`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Tag
	for rows.Next() {
		var t Tag
		if err := rows.Scan(&t.ID, &t.Name, &t.AgentCount); err != nil {
			return nil, err
		}
		out = append(out, t)
	}
	return out, rows.Err()
}

func (s *Store) CreateTag(ctx context.Context, name string) (Tag, error) {
	var t Tag
	err := s.Pool.QueryRow(ctx, `
		INSERT INTO tags(name) VALUES ($1) RETURNING id, name`, name).Scan(&t.ID, &t.Name)
	return t, err
}

func (s *Store) UpdateTag(ctx context.Context, id int64, name string) (Tag, error) {
	var t Tag
	err := s.Pool.QueryRow(ctx, `
		UPDATE tags SET name=$2 WHERE id=$1
		RETURNING id, name,
			(SELECT COUNT(*)::bigint FROM agent_tags WHERE tag_id=$1)`, id, name).
		Scan(&t.ID, &t.Name, &t.AgentCount)
	return t, err
}

func (s *Store) DeleteTag(ctx context.Context, id int64) error {
	_, err := s.Pool.Exec(ctx, `DELETE FROM tags WHERE id=$1`, id)
	return err
}

func (s *Store) isOnline(last *time.Time) bool {
	if last == nil {
		return false
	}
	return time.Since(*last) <= time.Duration(s.OnlineAfterS)*time.Second
}

func (s *Store) loadAgentExtras(ctx context.Context, a *Agent) error {
	ipRows, err := s.Pool.Query(ctx, `SELECT ipv4 FROM agent_ipv4s WHERE agent_id=$1 ORDER BY ipv4`, a.AgentID)
	if err != nil {
		return err
	}
	defer ipRows.Close()
	a.IPv4s = nil
	for ipRows.Next() {
		var ip string
		if err := ipRows.Scan(&ip); err != nil {
			return err
		}
		a.IPv4s = append(a.IPv4s, ip)
	}
	if a.IPv4s == nil {
		a.IPv4s = []string{}
	}
	tagRows, err := s.Pool.Query(ctx, `
		SELECT t.id, t.name FROM tags t
		JOIN agent_tags at ON at.tag_id=t.id
		WHERE at.agent_id=$1 ORDER BY t.name`, a.AgentID)
	if err != nil {
		return err
	}
	defer tagRows.Close()
	a.TagIDs = nil
	a.TagNames = nil
	for tagRows.Next() {
		var id int64
		var name string
		if err := tagRows.Scan(&id, &name); err != nil {
			return err
		}
		a.TagIDs = append(a.TagIDs, id)
		a.TagNames = append(a.TagNames, name)
	}
	if a.TagIDs == nil {
		a.TagIDs = []int64{}
	}
	if a.TagNames == nil {
		a.TagNames = []string{}
	}
	a.Online = s.isOnline(a.LastSeenAt)
	return nil
}

func scanAgent(row pgx.Row) (Agent, error) {
	var a Agent
	err := row.Scan(&a.AgentID, &a.DisplayName, &a.Hostname, &a.GroupID, &a.MediaPort, &a.Version,
		&a.PreferredIPv4, &a.LastSeenAt, &a.CreatedAt, &a.UpdatedAt)
	return a, err
}

const agentCols = `agent_id, display_name, hostname, group_id, media_port, version,
	preferred_ipv4, last_seen_at, created_at, updated_at`

func (s *Store) GetAgent(ctx context.Context, id string) (Agent, error) {
	a, err := scanAgent(s.Pool.QueryRow(ctx, `SELECT `+agentCols+` FROM agents WHERE agent_id=$1`, id))
	if err != nil {
		return a, err
	}
	if err := s.loadAgentExtras(ctx, &a); err != nil {
		return a, err
	}
	return a, nil
}

func (s *Store) ListAgents(ctx context.Context) ([]Agent, error) {
	rows, err := s.Pool.Query(ctx, `SELECT `+agentCols+` FROM agents ORDER BY display_name, hostname, agent_id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Agent
	for rows.Next() {
		var a Agent
		if err := rows.Scan(&a.AgentID, &a.DisplayName, &a.Hostname, &a.GroupID, &a.MediaPort, &a.Version,
			&a.PreferredIPv4, &a.LastSeenAt, &a.CreatedAt, &a.UpdatedAt); err != nil {
			return nil, err
		}
		if err := s.loadAgentExtras(ctx, &a); err != nil {
			return nil, err
		}
		out = append(out, a)
	}
	return out, rows.Err()
}

func (s *Store) UpsertRegister(ctx context.Context, agentID, hostname, version string, mediaPort int, ipv4s []string, preferred string) (Agent, error) {
	gid, err := s.UncategorizedGroupID(ctx)
	if err != nil {
		return Agent{}, err
	}
	now := time.Now().UTC()
	_, err = s.Pool.Exec(ctx, `
		INSERT INTO agents(agent_id, display_name, hostname, group_id, media_port, version, preferred_ipv4, last_seen_at, updated_at)
		VALUES ($1, $2, $2, $3, $4, $5, $6, $7, $7)
		ON CONFLICT (agent_id) DO UPDATE SET
			hostname=EXCLUDED.hostname,
			media_port=EXCLUDED.media_port,
			version=EXCLUDED.version,
			preferred_ipv4=EXCLUDED.preferred_ipv4,
			last_seen_at=EXCLUDED.last_seen_at,
			updated_at=EXCLUDED.updated_at`,
		agentID, hostname, gid, mediaPort, version, preferred, now)
	if err != nil {
		return Agent{}, err
	}
	if err := s.replaceIPv4s(ctx, agentID, ipv4s); err != nil {
		return Agent{}, err
	}
	return s.GetAgent(ctx, agentID)
}

func (s *Store) Heartbeat(ctx context.Context, agentID, hostname, version string, mediaPort int, ipv4s []string, preferred string) (Agent, error) {
	now := time.Now().UTC()
	ct, err := s.Pool.Exec(ctx, `
		UPDATE agents SET
			hostname=$2, media_port=$3, version=$4, preferred_ipv4=$5,
			last_seen_at=$6, updated_at=$6
		WHERE agent_id=$1`, agentID, hostname, mediaPort, version, preferred, now)
	if err != nil {
		return Agent{}, err
	}
	if ct.RowsAffected() == 0 {
		return s.UpsertRegister(ctx, agentID, hostname, version, mediaPort, ipv4s, preferred)
	}
	if err := s.replaceIPv4s(ctx, agentID, ipv4s); err != nil {
		return Agent{}, err
	}
	return s.GetAgent(ctx, agentID)
}

func (s *Store) replaceIPv4s(ctx context.Context, agentID string, ipv4s []string) error {
	tx, err := s.Pool.Begin(ctx)
	if err != nil {
		return err
	}
	defer tx.Rollback(ctx)
	if _, err := tx.Exec(ctx, `DELETE FROM agent_ipv4s WHERE agent_id=$1`, agentID); err != nil {
		return err
	}
	seen := map[string]struct{}{}
	for _, ip := range ipv4s {
		if ip == "" {
			continue
		}
		if _, ok := seen[ip]; ok {
			continue
		}
		seen[ip] = struct{}{}
		if _, err := tx.Exec(ctx, `INSERT INTO agent_ipv4s(agent_id, ipv4) VALUES ($1,$2)`, agentID, ip); err != nil {
			return err
		}
	}
	return tx.Commit(ctx)
}

func (s *Store) UpdateAgent(ctx context.Context, id string, displayName *string, groupID *int64, tagIDs *[]int64) (Agent, error) {
	if displayName != nil || groupID != nil {
		_, err := s.Pool.Exec(ctx, `
			UPDATE agents SET
				display_name = COALESCE($2, display_name),
				group_id = COALESCE($3, group_id),
				updated_at = NOW()
			WHERE agent_id=$1`, id, displayName, groupID)
		if err != nil {
			return Agent{}, err
		}
	}
	if tagIDs != nil {
		tx, err := s.Pool.Begin(ctx)
		if err != nil {
			return Agent{}, err
		}
		defer tx.Rollback(ctx)
		if _, err := tx.Exec(ctx, `DELETE FROM agent_tags WHERE agent_id=$1`, id); err != nil {
			return Agent{}, err
		}
		for _, tid := range *tagIDs {
			if _, err := tx.Exec(ctx, `INSERT INTO agent_tags(agent_id, tag_id) VALUES ($1,$2)`, id, tid); err != nil {
				return Agent{}, err
			}
		}
		if err := tx.Commit(ctx); err != nil {
			return Agent{}, err
		}
	}
	return s.GetAgent(ctx, id)
}

func (s *Store) DeleteAgent(ctx context.Context, id string) error {
	a, err := s.GetAgent(ctx, id)
	if err != nil {
		return err
	}
	if a.Online {
		return errors.New("不能删除在线 Agent")
	}
	_, err = s.Pool.Exec(ctx, `DELETE FROM agents WHERE agent_id=$1`, id)
	return err
}

type Department struct {
	ID        int64     `json:"id"`
	Name      string    `json:"name"`
	SortOrder int       `json:"sortOrder"`
	IsSystem  bool      `json:"isSystem"`
	UserCount int64     `json:"userCount"`
	CreatedAt time.Time `json:"createdAt"`
}

type User struct {
	ID             int64     `json:"id"`
	Username       string    `json:"username"`
	Role           string    `json:"role"`
	DepartmentID   int64     `json:"departmentId"`
	DepartmentName string    `json:"departmentName"`
	Enabled        bool      `json:"enabled"`
	CreatedAt      time.Time `json:"createdAt"`
	UpdatedAt      time.Time `json:"updatedAt"`
}

func (s *Store) ListDepartments(ctx context.Context) ([]Department, error) {
	rows, err := s.Pool.Query(ctx, `
		SELECT d.id, d.name, d.sort_order, d.is_system, d.created_at,
			COUNT(u.id)::bigint
		FROM departments d
		LEFT JOIN users u ON u.department_id = d.id
		GROUP BY d.id
		ORDER BY d.sort_order, d.id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Department
	for rows.Next() {
		var d Department
		if err := rows.Scan(&d.ID, &d.Name, &d.SortOrder, &d.IsSystem, &d.CreatedAt, &d.UserCount); err != nil {
			return nil, err
		}
		out = append(out, d)
	}
	return out, rows.Err()
}

func (s *Store) CreateDepartment(ctx context.Context, name string, sortOrder int) (Department, error) {
	var d Department
	err := s.Pool.QueryRow(ctx, `
		INSERT INTO departments(name, sort_order) VALUES ($1,$2)
		RETURNING id, name, sort_order, is_system, created_at`, name, sortOrder).
		Scan(&d.ID, &d.Name, &d.SortOrder, &d.IsSystem, &d.CreatedAt)
	d.UserCount = 0
	return d, err
}

func (s *Store) UpdateDepartment(ctx context.Context, id int64, name *string, sortOrder *int) (Department, error) {
	var d Department
	err := s.Pool.QueryRow(ctx, `
		UPDATE departments SET
			name = COALESCE($2, name),
			sort_order = COALESCE($3, sort_order)
		WHERE id=$1
		RETURNING id, name, sort_order, is_system, created_at`, id, name, sortOrder).
		Scan(&d.ID, &d.Name, &d.SortOrder, &d.IsSystem, &d.CreatedAt)
	if err != nil {
		return d, err
	}
	_ = s.Pool.QueryRow(ctx, `SELECT COUNT(*) FROM users WHERE department_id=$1`, id).Scan(&d.UserCount)
	return d, nil
}

func (s *Store) DeleteDepartment(ctx context.Context, id int64) error {
	var isSystem bool
	var n int
	err := s.Pool.QueryRow(ctx, `SELECT is_system FROM departments WHERE id=$1`, id).Scan(&isSystem)
	if err != nil {
		return err
	}
	if isSystem {
		return errors.New("不能删除系统部门")
	}
	if err := s.Pool.QueryRow(ctx, `SELECT COUNT(*) FROM users WHERE department_id=$1`, id).Scan(&n); err != nil {
		return err
	}
	if n > 0 {
		return errors.New("部门下仍有用户")
	}
	_, err = s.Pool.Exec(ctx, `DELETE FROM departments WHERE id=$1`, id)
	return err
}

func (s *Store) ListUsers(ctx context.Context) ([]User, error) {
	rows, err := s.Pool.Query(ctx, `
		SELECT u.id, u.username, u.role, u.department_id, d.name, u.enabled, u.created_at, u.updated_at
		FROM users u
		JOIN departments d ON d.id = u.department_id
		ORDER BY u.username`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []User
	for rows.Next() {
		var u User
		if err := rows.Scan(&u.ID, &u.Username, &u.Role, &u.DepartmentID, &u.DepartmentName,
			&u.Enabled, &u.CreatedAt, &u.UpdatedAt); err != nil {
			return nil, err
		}
		out = append(out, u)
	}
	return out, rows.Err()
}

func (s *Store) GetUser(ctx context.Context, id int64) (User, error) {
	var u User
	err := s.Pool.QueryRow(ctx, `
		SELECT u.id, u.username, u.role, u.department_id, d.name, u.enabled, u.created_at, u.updated_at
		FROM users u
		JOIN departments d ON d.id = u.department_id
		WHERE u.id=$1`, id).
		Scan(&u.ID, &u.Username, &u.Role, &u.DepartmentID, &u.DepartmentName,
			&u.Enabled, &u.CreatedAt, &u.UpdatedAt)
	return u, err
}

func (s *Store) CreateUser(ctx context.Context, username, passwordHash, role string, departmentID int64, enabled bool) (User, error) {
	if role != "admin" && role != "viewer" {
		return User{}, errors.New("角色必须是管理员或操作员")
	}
	var id int64
	err := s.Pool.QueryRow(ctx, `
		INSERT INTO users(username, password_hash, role, department_id, enabled)
		VALUES ($1,$2,$3,$4,$5)
		RETURNING id`, username, passwordHash, role, departmentID, enabled).Scan(&id)
	if err != nil {
		return User{}, err
	}
	return s.GetUser(ctx, id)
}

func (s *Store) UpdateUser(ctx context.Context, id int64, passwordHash *string, role *string, departmentID *int64, enabled *bool) (User, error) {
	cur, err := s.GetUser(ctx, id)
	if err != nil {
		return User{}, err
	}
	newRole := cur.Role
	if role != nil {
		if *role != "admin" && *role != "viewer" {
			return User{}, errors.New("角色必须是管理员或操作员")
		}
		newRole = *role
	}
	newEnabled := cur.Enabled
	if enabled != nil {
		newEnabled = *enabled
	}
	if cur.Role == "admin" && (newRole != "admin" || !newEnabled) {
		var n int
		if err := s.Pool.QueryRow(ctx, `
			SELECT COUNT(*) FROM users WHERE role='admin' AND enabled AND id<>$1`, id).Scan(&n); err != nil {
			return User{}, err
		}
		if n == 0 {
			return User{}, errors.New("不能停用或降级最后一位启用的管理员")
		}
	}
	_, err = s.Pool.Exec(ctx, `
		UPDATE users SET
			password_hash = COALESCE($2, password_hash),
			role = COALESCE($3, role),
			department_id = COALESCE($4, department_id),
			enabled = COALESCE($5, enabled),
			updated_at = NOW()
		WHERE id=$1`, id, passwordHash, role, departmentID, enabled)
	if err != nil {
		return User{}, err
	}
	return s.GetUser(ctx, id)
}

func (s *Store) DeleteUser(ctx context.Context, id int64) error {
	cur, err := s.GetUser(ctx, id)
	if err != nil {
		return err
	}
	if cur.Role == "admin" && cur.Enabled {
		var n int
		if err := s.Pool.QueryRow(ctx, `
			SELECT COUNT(*) FROM users WHERE role='admin' AND enabled AND id<>$1`, id).Scan(&n); err != nil {
			return err
		}
		if n == 0 {
			return errors.New("不能删除最后一位启用的管理员")
		}
	}
	_, err = s.Pool.Exec(ctx, `DELETE FROM users WHERE id=$1`, id)
	return err
}
