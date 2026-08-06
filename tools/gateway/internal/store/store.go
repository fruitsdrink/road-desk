package store

import (
	"context"
	"encoding/json"
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
	ID        int64  `json:"id"`
	ParentID  *int64 `json:"parentId"`
	Name      string `json:"name"`
	SortOrder int    `json:"sortOrder"`
	IsSystem  bool   `json:"isSystem"`
}

type Tag struct {
	ID         int64  `json:"id"`
	Name       string `json:"name"`
	AgentCount int64  `json:"agentCount"`
}

type ComputerRole struct {
	ID         int64     `json:"id"`
	Name       string    `json:"name"`
	SortOrder  int       `json:"sortOrder"`
	AgentCount int64     `json:"agentCount"`
	CreatedAt  time.Time `json:"createdAt"`
}

type Agent struct {
	AgentID       string          `json:"agentId"`
	DisplayName   string          `json:"displayName"`
	Hostname      string          `json:"hostname"`
	GroupID       int64           `json:"groupId"`
	MediaPort     int             `json:"mediaPort"`
	Version       string          `json:"version"`
	PreferredIPv4    string          `json:"preferredIpv4"`
	ComputerRole     string          `json:"computerRole"`
	InstallLocation  string          `json:"installLocation"`
	LaneNumber       string          `json:"laneNumber"`
	IPv4s            []string        `json:"ipv4s"`
	TagIDs           []int64         `json:"tagIds"`
	TagNames         []string        `json:"tagNames,omitempty"`
	Inventory        json.RawMessage `json:"inventory,omitempty"`
	LastSeenAt       *time.Time      `json:"lastSeenAt"`
	Online           bool            `json:"online"`
	CreatedAt        time.Time       `json:"createdAt"`
	UpdatedAt        time.Time       `json:"updatedAt"`
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

func (s *Store) nextSiblingSort(ctx context.Context, parentID *int64) (int, error) {
	var n int
	var err error
	if parentID == nil {
		err = s.Pool.QueryRow(ctx, `
			SELECT COALESCE(MAX(sort_order), -1) + 1 FROM groups WHERE parent_id IS NULL`).Scan(&n)
	} else {
		err = s.Pool.QueryRow(ctx, `
			SELECT COALESCE(MAX(sort_order), -1) + 1 FROM groups WHERE parent_id=$1`, *parentID).Scan(&n)
	}
	return n, err
}

func (s *Store) CreateGroup(ctx context.Context, parentID *int64, name string, sortOrder int) (Group, error) {
	if sortOrder <= 0 {
		so, err := s.nextSiblingSort(ctx, parentID)
		if err != nil {
			return Group{}, err
		}
		sortOrder = so
	}
	var g Group
	err := s.Pool.QueryRow(ctx, `
		INSERT INTO groups(parent_id, name, sort_order) VALUES ($1,$2,$3)
		RETURNING id, parent_id, name, sort_order, is_system`, parentID, name, sortOrder).
		Scan(&g.ID, &g.ParentID, &g.Name, &g.SortOrder, &g.IsSystem)
	return g, err
}

func (s *Store) validateGroupParent(ctx context.Context, id int64, parentID *int64) error {
	if parentID == nil {
		return nil
	}
	if *parentID == id {
		return errors.New("不能将分组设为自己的父级")
	}
	var isDesc bool
	err := s.Pool.QueryRow(ctx, `
		WITH RECURSIVE subtree AS (
			SELECT id FROM groups WHERE parent_id=$1
			UNION ALL
			SELECT g.id FROM groups g JOIN subtree s ON g.parent_id = s.id
		)
		SELECT EXISTS(SELECT 1 FROM subtree WHERE id=$2)`, id, *parentID).Scan(&isDesc)
	if err != nil {
		return err
	}
	if isDesc {
		return errors.New("不能将分组移动到自己的子分组下")
	}
	var exists bool
	if err := s.Pool.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM groups WHERE id=$1)`, *parentID).Scan(&exists); err != nil {
		return err
	}
	if !exists {
		return errors.New("父分组不存在")
	}
	return nil
}

func (s *Store) UpdateGroup(ctx context.Context, id int64, parentID *int64, setParent bool, name *string, sortOrder *int) (Group, error) {
	if setParent {
		if err := s.validateGroupParent(ctx, id, parentID); err != nil {
			return Group{}, err
		}
	}

	var g Group
	var err error
	if setParent {
		err = s.Pool.QueryRow(ctx, `
			UPDATE groups SET
				parent_id = $2,
				name = COALESCE($3, name),
				sort_order = COALESCE($4, sort_order)
			WHERE id=$1
			RETURNING id, parent_id, name, sort_order, is_system`, id, parentID, name, sortOrder).
			Scan(&g.ID, &g.ParentID, &g.Name, &g.SortOrder, &g.IsSystem)
	} else {
		err = s.Pool.QueryRow(ctx, `
			UPDATE groups SET
				name = COALESCE($2, name),
				sort_order = COALESCE($3, sort_order)
			WHERE id=$1
			RETURNING id, parent_id, name, sort_order, is_system`, id, name, sortOrder).
			Scan(&g.ID, &g.ParentID, &g.Name, &g.SortOrder, &g.IsSystem)
	}
	return g, err
}

// MoveGroup sets parent and sibling index (0-based) for a group, renumbering sort_order.
func (s *Store) MoveGroup(ctx context.Context, id int64, parentID *int64, index int) (Group, error) {
	var exists bool
	if err := s.Pool.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM groups WHERE id=$1)`, id).Scan(&exists); err != nil {
		return Group{}, err
	}
	if !exists {
		return Group{}, pgx.ErrNoRows
	}
	if err := s.validateGroupParent(ctx, id, parentID); err != nil {
		return Group{}, err
	}

	tx, err := s.Pool.Begin(ctx)
	if err != nil {
		return Group{}, err
	}
	defer tx.Rollback(ctx)

	var siblingIDs []int64
	var rows pgx.Rows
	if parentID == nil {
		rows, err = tx.Query(ctx, `
			SELECT id FROM groups
			WHERE parent_id IS NULL AND id<>$1
			ORDER BY sort_order, id`, id)
	} else {
		rows, err = tx.Query(ctx, `
			SELECT id FROM groups
			WHERE parent_id=$1 AND id<>$2
			ORDER BY sort_order, id`, *parentID, id)
	}
	if err != nil {
		return Group{}, err
	}
	for rows.Next() {
		var sid int64
		if err := rows.Scan(&sid); err != nil {
			rows.Close()
			return Group{}, err
		}
		siblingIDs = append(siblingIDs, sid)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return Group{}, err
	}

	if index < 0 {
		index = 0
	}
	if index > len(siblingIDs) {
		index = len(siblingIDs)
	}
	ordered := make([]int64, 0, len(siblingIDs)+1)
	ordered = append(ordered, siblingIDs[:index]...)
	ordered = append(ordered, id)
	ordered = append(ordered, siblingIDs[index:]...)

	if _, err := tx.Exec(ctx, `UPDATE groups SET parent_id=$2 WHERE id=$1`, id, parentID); err != nil {
		return Group{}, err
	}
	for i, sid := range ordered {
		if _, err := tx.Exec(ctx, `UPDATE groups SET sort_order=$2 WHERE id=$1`, sid, i); err != nil {
			return Group{}, err
		}
	}
	if err := tx.Commit(ctx); err != nil {
		return Group{}, err
	}
	return s.GetGroup(ctx, id)
}

func (s *Store) GetGroup(ctx context.Context, id int64) (Group, error) {
	var g Group
	err := s.Pool.QueryRow(ctx, `
		SELECT id, parent_id, name, sort_order, is_system FROM groups WHERE id=$1`, id).
		Scan(&g.ID, &g.ParentID, &g.Name, &g.SortOrder, &g.IsSystem)
	return g, err
}

func (s *Store) DeleteGroup(ctx context.Context, id int64, cascade bool) error {
	var isSystem bool
	err := s.Pool.QueryRow(ctx, `SELECT is_system FROM groups WHERE id=$1`, id).Scan(&isSystem)
	if err != nil {
		return err
	}
	if isSystem {
		return errors.New("不能删除系统分组")
	}

	var childN, agentN int
	if err := s.Pool.QueryRow(ctx, `SELECT COUNT(*) FROM groups WHERE parent_id=$1`, id).Scan(&childN); err != nil {
		return err
	}
	if err := s.Pool.QueryRow(ctx, `
		WITH RECURSIVE subtree AS (
			SELECT id FROM groups WHERE id=$1
			UNION ALL
			SELECT g.id FROM groups g JOIN subtree s ON g.parent_id = s.id
		)
		SELECT COUNT(*) FROM agents WHERE group_id IN (SELECT id FROM subtree)`, id).Scan(&agentN); err != nil {
		return err
	}

	if !cascade && (childN > 0 || agentN > 0) {
		if childN > 0 && agentN > 0 {
			return errors.New("分组下仍有子分组与 Agent，请确认后级联删除")
		}
		if childN > 0 {
			return errors.New("分组下仍有子分组，请确认后级联删除")
		}
		return errors.New("分组下仍有 Agent，请确认后级联删除")
	}

	if cascade && agentN > 0 {
		uncat, err := s.UncategorizedGroupID(ctx)
		if err != nil {
			return err
		}
		if _, err := s.Pool.Exec(ctx, `
			WITH RECURSIVE subtree AS (
				SELECT id FROM groups WHERE id=$1
				UNION ALL
				SELECT g.id FROM groups g JOIN subtree s ON g.parent_id = s.id
			)
			UPDATE agents SET group_id=$2, updated_at=NOW()
			WHERE group_id IN (SELECT id FROM subtree) AND group_id<>$2`, id, uncat); err != nil {
			return err
		}
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

func (s *Store) ListComputerRoles(ctx context.Context) ([]ComputerRole, error) {
	rows, err := s.Pool.Query(ctx, `
		SELECT r.id, r.name, r.sort_order, r.created_at,
			COUNT(a.agent_id)::bigint
		FROM computer_roles r
		LEFT JOIN agents a ON a.computer_role = r.name
		GROUP BY r.id
		ORDER BY r.sort_order, r.id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []ComputerRole
	for rows.Next() {
		var r ComputerRole
		if err := rows.Scan(&r.ID, &r.Name, &r.SortOrder, &r.CreatedAt, &r.AgentCount); err != nil {
			return nil, err
		}
		out = append(out, r)
	}
	return out, rows.Err()
}

func (s *Store) CreateComputerRole(ctx context.Context, name string, sortOrder int) (ComputerRole, error) {
	var r ComputerRole
	err := s.Pool.QueryRow(ctx, `
		INSERT INTO computer_roles(name, sort_order) VALUES ($1,$2)
		RETURNING id, name, sort_order, created_at`, name, sortOrder).
		Scan(&r.ID, &r.Name, &r.SortOrder, &r.CreatedAt)
	r.AgentCount = 0
	return r, err
}

func (s *Store) UpdateComputerRole(ctx context.Context, id int64, name *string, sortOrder *int) (ComputerRole, error) {
	tx, err := s.Pool.Begin(ctx)
	if err != nil {
		return ComputerRole{}, err
	}
	defer tx.Rollback(ctx)

	var oldName string
	if err := tx.QueryRow(ctx, `SELECT name FROM computer_roles WHERE id=$1`, id).Scan(&oldName); err != nil {
		return ComputerRole{}, err
	}

	var r ComputerRole
	err = tx.QueryRow(ctx, `
		UPDATE computer_roles SET
			name = COALESCE($2, name),
			sort_order = COALESCE($3, sort_order)
		WHERE id=$1
		RETURNING id, name, sort_order, created_at`, id, name, sortOrder).
		Scan(&r.ID, &r.Name, &r.SortOrder, &r.CreatedAt)
	if err != nil {
		return ComputerRole{}, err
	}

	if name != nil && *name != oldName {
		if _, err := tx.Exec(ctx, `
			UPDATE agents SET computer_role=$2, updated_at=NOW()
			WHERE computer_role=$1`, oldName, *name); err != nil {
			return ComputerRole{}, err
		}
	}

	if err := tx.QueryRow(ctx, `
		SELECT COUNT(*)::bigint FROM agents WHERE computer_role=$1`, r.Name).Scan(&r.AgentCount); err != nil {
		return ComputerRole{}, err
	}
	if err := tx.Commit(ctx); err != nil {
		return ComputerRole{}, err
	}
	return r, nil
}

func (s *Store) DeleteComputerRole(ctx context.Context, id int64) error {
	var name string
	var n int64
	err := s.Pool.QueryRow(ctx, `
		SELECT r.name, COUNT(a.agent_id)::bigint
		FROM computer_roles r
		LEFT JOIN agents a ON a.computer_role = r.name
		WHERE r.id=$1
		GROUP BY r.id, r.name`, id).Scan(&name, &n)
	if err != nil {
		return err
	}
	if n > 0 {
		return errors.New("仍有 Agent 使用该角色")
	}
	_, err = s.Pool.Exec(ctx, `DELETE FROM computer_roles WHERE id=$1`, id)
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
	var inv []byte
	err := row.Scan(&a.AgentID, &a.DisplayName, &a.Hostname, &a.GroupID, &a.MediaPort, &a.Version,
		&a.PreferredIPv4, &a.ComputerRole, &a.InstallLocation, &a.LaneNumber, &inv, &a.LastSeenAt,
		&a.CreatedAt, &a.UpdatedAt)
	if err != nil {
		return a, err
	}
	if len(inv) == 0 {
		a.Inventory = json.RawMessage(`{}`)
	} else {
		a.Inventory = json.RawMessage(inv)
	}
	return a, nil
}

const agentCols = `agent_id, display_name, hostname, group_id, media_port, version,
	preferred_ipv4, COALESCE(computer_role, ''), COALESCE(install_location, ''),
	COALESCE(lane_number, ''), COALESCE(inventory, '{}'::jsonb),
	last_seen_at, created_at, updated_at`

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
		var inv []byte
		if err := rows.Scan(&a.AgentID, &a.DisplayName, &a.Hostname, &a.GroupID, &a.MediaPort, &a.Version,
			&a.PreferredIPv4, &a.ComputerRole, &a.InstallLocation, &a.LaneNumber, &inv, &a.LastSeenAt,
			&a.CreatedAt, &a.UpdatedAt); err != nil {
			return nil, err
		}
		if len(inv) == 0 {
			a.Inventory = json.RawMessage(`{}`)
		} else {
			a.Inventory = json.RawMessage(inv)
		}
		if err := s.loadAgentExtras(ctx, &a); err != nil {
			return nil, err
		}
		out = append(out, a)
	}
	return out, rows.Err()
}

func (s *Store) UpsertRegister(ctx context.Context, agentID, hostname, version string, mediaPort int, ipv4s []string, preferred string, inventory json.RawMessage) (Agent, error) {
	gid, err := s.UncategorizedGroupID(ctx)
	if err != nil {
		return Agent{}, err
	}
	now := time.Now().UTC()
	inv := []byte(`{}`)
	if len(inventory) > 0 && json.Valid(inventory) {
		inv = inventory
	}
	_, err = s.Pool.Exec(ctx, `
		INSERT INTO agents(agent_id, display_name, hostname, group_id, media_port, version, preferred_ipv4, inventory, last_seen_at, updated_at)
		VALUES ($1, $2, $2, $3, $4, $5, $6, $7::jsonb, $8, $8)
		ON CONFLICT (agent_id) DO UPDATE SET
			hostname=EXCLUDED.hostname,
			media_port=EXCLUDED.media_port,
			version=EXCLUDED.version,
			preferred_ipv4=EXCLUDED.preferred_ipv4,
			inventory=EXCLUDED.inventory,
			last_seen_at=EXCLUDED.last_seen_at,
			updated_at=EXCLUDED.updated_at`,
		agentID, hostname, gid, mediaPort, version, preferred, inv, now)
	if err != nil {
		return Agent{}, err
	}
	if err := s.replaceIPv4s(ctx, agentID, ipv4s); err != nil {
		return Agent{}, err
	}
	return s.GetAgent(ctx, agentID)
}

func (s *Store) Heartbeat(ctx context.Context, agentID, hostname, version string, mediaPort int, ipv4s []string, preferred string, inventory json.RawMessage) (Agent, error) {
	now := time.Now().UTC()
	inv := []byte(`{}`)
	if len(inventory) > 0 && json.Valid(inventory) {
		inv = inventory
	}
	ct, err := s.Pool.Exec(ctx, `
		UPDATE agents SET
			hostname=$2, media_port=$3, version=$4, preferred_ipv4=$5, inventory=$6::jsonb,
			last_seen_at=$7, updated_at=$7
		WHERE agent_id=$1`, agentID, hostname, mediaPort, version, preferred, inv, now)
	if err != nil {
		return Agent{}, err
	}
	if ct.RowsAffected() == 0 {
		return s.UpsertRegister(ctx, agentID, hostname, version, mediaPort, ipv4s, preferred, inventory)
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

func (s *Store) UpdateAgent(ctx context.Context, id string, displayName *string, groupID *int64, computerRole *string, installLocation *string, laneNumber *string, tagIDs *[]int64) (Agent, error) {
	if displayName != nil || groupID != nil || computerRole != nil || installLocation != nil || laneNumber != nil {
		_, err := s.Pool.Exec(ctx, `
			UPDATE agents SET
				display_name = COALESCE($2, display_name),
				group_id = COALESCE($3, group_id),
				computer_role = COALESCE($4, computer_role),
				install_location = COALESCE($5, install_location),
				lane_number = COALESCE($6, lane_number),
				updated_at = NOW()
			WHERE agent_id=$1`, id, displayName, groupID, computerRole, installLocation, laneNumber)
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

type ViewerPresence struct {
	ViewerID   string     `json:"viewerId"`
	Hostname   string     `json:"hostname"`
	Username   string     `json:"username"`
	AuthMode   string     `json:"authMode"`
	Version    string     `json:"version"`
	ClientIP   string     `json:"clientIp"`
	Online     bool       `json:"online"`
	LastSeenAt *time.Time `json:"lastSeenAt"`
	CreatedAt  time.Time  `json:"createdAt"`
	UpdatedAt  time.Time  `json:"updatedAt"`
}

func (s *Store) UpsertViewerPresence(ctx context.Context, viewerID, hostname, username, authMode, version, clientIP string) (ViewerPresence, error) {
	var v ViewerPresence
	err := s.Pool.QueryRow(ctx, `
		INSERT INTO viewer_presence(viewer_id, hostname, username, auth_mode, version, client_ip, last_seen_at, updated_at)
		VALUES ($1,$2,$3,$4,$5,$6,NOW(),NOW())
		ON CONFLICT (viewer_id) DO UPDATE SET
			hostname = EXCLUDED.hostname,
			username = EXCLUDED.username,
			auth_mode = EXCLUDED.auth_mode,
			version = EXCLUDED.version,
			client_ip = EXCLUDED.client_ip,
			last_seen_at = NOW(),
			updated_at = NOW()
		RETURNING viewer_id, hostname, username, auth_mode, version, client_ip,
			last_seen_at, created_at, updated_at`,
		viewerID, hostname, username, authMode, version, clientIP).
		Scan(&v.ViewerID, &v.Hostname, &v.Username, &v.AuthMode, &v.Version, &v.ClientIP,
			&v.LastSeenAt, &v.CreatedAt, &v.UpdatedAt)
	if err != nil {
		return v, err
	}
	v.Online = s.isOnline(v.LastSeenAt)
	return v, nil
}

func (s *Store) MarkViewerOffline(ctx context.Context, viewerID string) error {
	tag, err := s.Pool.Exec(ctx, `
		UPDATE viewer_presence
		SET last_seen_at = NULL, updated_at = NOW()
		WHERE viewer_id=$1`, viewerID)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return pgx.ErrNoRows
	}
	return nil
}

func (s *Store) ListViewerPresence(ctx context.Context) ([]ViewerPresence, error) {
	rows, err := s.Pool.Query(ctx, `
		SELECT viewer_id, hostname, username, auth_mode, version, client_ip,
			last_seen_at, created_at, updated_at
		FROM viewer_presence
		ORDER BY last_seen_at DESC NULLS LAST, viewer_id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []ViewerPresence
	for rows.Next() {
		var v ViewerPresence
		if err := rows.Scan(&v.ViewerID, &v.Hostname, &v.Username, &v.AuthMode, &v.Version, &v.ClientIP,
			&v.LastSeenAt, &v.CreatedAt, &v.UpdatedAt); err != nil {
			return nil, err
		}
		v.Online = s.isOnline(v.LastSeenAt)
		out = append(out, v)
	}
	return out, rows.Err()
}
