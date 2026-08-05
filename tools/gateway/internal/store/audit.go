package store

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
)

type AuditSession struct {
	ID                string          `json:"id"`
	OperatorUserID    *int64          `json:"operatorUserId"`
	OperatorName      string          `json:"operatorName"`
	ViewerHost        string          `json:"viewerHost"`
	ViewerIP          string          `json:"viewerIp"`
	AgentID           string          `json:"agentId"`
	AgentName         string          `json:"agentName"`
	AgentEndpoint     string          `json:"agentEndpoint"`
	Mode              string          `json:"mode"`
	Result            string          `json:"result"`
	DisconnectReason  string          `json:"disconnectReason"`
	UsedClipboard     bool            `json:"usedClipboard"`
	UsedFileTransfer  bool            `json:"usedFileTransfer"`
	AttemptedAt       time.Time       `json:"attemptedAt"`
	OpenedAt          *time.Time      `json:"openedAt"`
	ClosedAt          *time.Time      `json:"closedAt"`
	Partial           bool            `json:"partial"`
	Meta              json.RawMessage `json:"meta"`
	CreatedAt         time.Time       `json:"createdAt"`
	UpdatedAt         time.Time       `json:"updatedAt"`
}

// AuditUpsert is a partial update; empty strings / nil leave existing values
// (except clipboard/file flags which OR, and result which replaces when non-empty).
type AuditUpsert struct {
	ID               string
	OperatorUserID   *int64
	OperatorName     *string
	ViewerHost       *string
	ViewerIP         *string
	AgentID          *string
	AgentName        *string
	AgentEndpoint    *string
	Mode             *string
	Result           *string
	DisconnectReason *string
	UsedClipboard    *bool
	UsedFileTransfer *bool
	AttemptedAt      *time.Time
	OpenedAt         *time.Time
	ClosedAt         *time.Time
	Partial          *bool
	Meta             json.RawMessage
}

type AuditListFilter struct {
	From       *time.Time
	To         *time.Time
	AgentID    string
	Operator   string
	Result     string
	Limit      int
	CursorAt   *time.Time
	CursorID   string
}

var (
	allowedModes = map[string]bool{
		"control": true, "view_only": true, "unknown": true,
	}
	allowedResults = map[string]bool{
		"ok": true, "auth_fail": true, "capacity_reject": true,
		"connect_fail": true, "tls_fail": true, "cancelled": true, "unknown": true,
	}
)

func ValidateAuditMode(m string) bool   { return allowedModes[m] }
func ValidateAuditResult(r string) bool { return allowedResults[r] }

func scanAuditSession(row pgx.Row) (AuditSession, error) {
	var a AuditSession
	var meta []byte
	err := row.Scan(
		&a.ID, &a.OperatorUserID, &a.OperatorName, &a.ViewerHost, &a.ViewerIP,
		&a.AgentID, &a.AgentName, &a.AgentEndpoint, &a.Mode, &a.Result, &a.DisconnectReason,
		&a.UsedClipboard, &a.UsedFileTransfer, &a.AttemptedAt, &a.OpenedAt, &a.ClosedAt,
		&a.Partial, &meta, &a.CreatedAt, &a.UpdatedAt,
	)
	if err != nil {
		return a, err
	}
	if len(meta) == 0 {
		a.Meta = json.RawMessage(`{}`)
	} else {
		a.Meta = json.RawMessage(meta)
	}
	return a, nil
}

const auditSelectCols = `
	id, operator_user_id, operator_name, viewer_host, viewer_ip,
	agent_id, agent_name, agent_endpoint, mode, result, disconnect_reason,
	used_clipboard, used_file_transfer, attempted_at, opened_at, closed_at,
	partial, meta, created_at, updated_at`

func (s *Store) GetAuditSession(ctx context.Context, id string) (AuditSession, error) {
	row := s.Pool.QueryRow(ctx, `SELECT `+auditSelectCols+` FROM audit_sessions WHERE id=$1`, id)
	a, err := scanAuditSession(row)
	if errors.Is(err, pgx.ErrNoRows) {
		return a, pgx.ErrNoRows
	}
	return a, err
}

func (s *Store) UpsertAuditSession(ctx context.Context, u AuditUpsert) (AuditSession, error) {
	if u.ID == "" {
		return AuditSession{}, errors.New("需要 session id")
	}
	mode := "unknown"
	if u.Mode != nil && *u.Mode != "" {
		if !ValidateAuditMode(*u.Mode) {
			return AuditSession{}, fmt.Errorf("无效的 mode: %s", *u.Mode)
		}
		mode = *u.Mode
	}
	result := "unknown"
	if u.Result != nil && *u.Result != "" {
		if !ValidateAuditResult(*u.Result) {
			return AuditSession{}, fmt.Errorf("无效的 result: %s", *u.Result)
		}
		result = *u.Result
	}
	attempted := time.Now().UTC()
	if u.AttemptedAt != nil {
		attempted = u.AttemptedAt.UTC()
	}
	opName, viewerHost, viewerIP := "", "", ""
	agentID, agentName, agentEndpoint, disc := "", "", "", ""
	if u.OperatorName != nil {
		opName = *u.OperatorName
	}
	if u.ViewerHost != nil {
		viewerHost = *u.ViewerHost
	}
	if u.ViewerIP != nil {
		viewerIP = *u.ViewerIP
	}
	if u.AgentID != nil {
		agentID = *u.AgentID
	}
	if u.AgentName != nil {
		agentName = *u.AgentName
	}
	if u.AgentEndpoint != nil {
		agentEndpoint = *u.AgentEndpoint
	}
	if u.DisconnectReason != nil {
		disc = *u.DisconnectReason
	}
	usedClip, usedFile, partial := false, false, true
	if u.UsedClipboard != nil {
		usedClip = *u.UsedClipboard
	}
	if u.UsedFileTransfer != nil {
		usedFile = *u.UsedFileTransfer
	}
	if u.Partial != nil {
		partial = *u.Partial
	}
	meta := []byte(`{}`)
	if len(u.Meta) > 0 {
		if !json.Valid(u.Meta) {
			return AuditSession{}, errors.New("meta 须为 JSON 对象")
		}
		meta = u.Meta
	}

	row := s.Pool.QueryRow(ctx, `
		INSERT INTO audit_sessions (
			id, operator_user_id, operator_name, viewer_host, viewer_ip,
			agent_id, agent_name, agent_endpoint, mode, result, disconnect_reason,
			used_clipboard, used_file_transfer, attempted_at, opened_at, closed_at,
			partial, meta
		) VALUES (
			$1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18::jsonb
		)
		ON CONFLICT (id) DO UPDATE SET
			operator_user_id = COALESCE(EXCLUDED.operator_user_id, audit_sessions.operator_user_id),
			operator_name = CASE WHEN EXCLUDED.operator_name <> '' THEN EXCLUDED.operator_name ELSE audit_sessions.operator_name END,
			viewer_host = CASE WHEN EXCLUDED.viewer_host <> '' THEN EXCLUDED.viewer_host ELSE audit_sessions.viewer_host END,
			viewer_ip = CASE WHEN EXCLUDED.viewer_ip <> '' THEN EXCLUDED.viewer_ip ELSE audit_sessions.viewer_ip END,
			agent_id = CASE WHEN EXCLUDED.agent_id <> '' THEN EXCLUDED.agent_id ELSE audit_sessions.agent_id END,
			agent_name = CASE WHEN EXCLUDED.agent_name <> '' THEN EXCLUDED.agent_name ELSE audit_sessions.agent_name END,
			agent_endpoint = CASE WHEN EXCLUDED.agent_endpoint <> '' THEN EXCLUDED.agent_endpoint ELSE audit_sessions.agent_endpoint END,
			mode = CASE WHEN EXCLUDED.mode <> 'unknown' THEN EXCLUDED.mode
			            WHEN audit_sessions.mode = 'unknown' THEN EXCLUDED.mode
			            ELSE audit_sessions.mode END,
			result = CASE WHEN EXCLUDED.result <> 'unknown' THEN EXCLUDED.result
			              WHEN audit_sessions.result = 'unknown' THEN EXCLUDED.result
			              ELSE audit_sessions.result END,
			disconnect_reason = CASE WHEN EXCLUDED.disconnect_reason <> '' THEN EXCLUDED.disconnect_reason ELSE audit_sessions.disconnect_reason END,
			used_clipboard = audit_sessions.used_clipboard OR EXCLUDED.used_clipboard,
			used_file_transfer = audit_sessions.used_file_transfer OR EXCLUDED.used_file_transfer,
			attempted_at = LEAST(audit_sessions.attempted_at, EXCLUDED.attempted_at),
			opened_at = COALESCE(audit_sessions.opened_at, EXCLUDED.opened_at),
			closed_at = COALESCE(EXCLUDED.closed_at, audit_sessions.closed_at),
			partial = CASE WHEN $19::boolean IS NOT NULL THEN $19::boolean ELSE audit_sessions.partial END,
			meta = COALESCE(audit_sessions.meta, '{}'::jsonb) || COALESCE(EXCLUDED.meta, '{}'::jsonb),
			updated_at = NOW()
		RETURNING `+auditSelectCols,
		u.ID, u.OperatorUserID, opName, viewerHost, viewerIP,
		agentID, agentName, agentEndpoint, mode, result, disc,
		usedClip, usedFile, attempted, u.OpenedAt, u.ClosedAt,
		partial, meta, u.Partial,
	)
	return scanAuditSession(row)
}

func (s *Store) ListAuditSessions(ctx context.Context, f AuditListFilter) ([]AuditSession, error) {
	limit := f.Limit
	if limit <= 0 || limit > 200 {
		limit = 50
	}
	var (
		b    strings.Builder
		args []any
		n    int
	)
	arg := func(v any) string {
		n++
		args = append(args, v)
		return fmt.Sprintf("$%d", n)
	}
	b.WriteString(`SELECT ` + auditSelectCols + ` FROM audit_sessions WHERE TRUE`)
	if f.From != nil {
		b.WriteString(` AND attempted_at >= ` + arg(f.From.UTC()))
	}
	if f.To != nil {
		b.WriteString(` AND attempted_at < ` + arg(f.To.UTC()))
	}
	if f.AgentID != "" {
		b.WriteString(` AND agent_id = ` + arg(f.AgentID))
	}
	if f.Operator != "" {
		b.WriteString(` AND operator_name ILIKE ` + arg("%"+f.Operator+"%"))
	}
	if f.Result != "" {
		b.WriteString(` AND result = ` + arg(f.Result))
	}
	if f.CursorAt != nil && f.CursorID != "" {
		// (attempted_at, id) < cursor for DESC pagination
		b.WriteString(` AND (attempted_at, id::text) < (` + arg(f.CursorAt.UTC()) + `, ` + arg(f.CursorID) + `)`)
	}
	b.WriteString(` ORDER BY attempted_at DESC, id DESC LIMIT ` + arg(limit))

	rows, err := s.Pool.Query(ctx, b.String(), args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []AuditSession
	for rows.Next() {
		a, err := scanAuditSession(rows)
		if err != nil {
			return nil, err
		}
		out = append(out, a)
	}
	return out, rows.Err()
}
