package store

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
)

// Audit event sources and types — keep in sync with docs/audit-implementation-plan.md §4.2.
const (
	AuditSourceViewer  = "viewer"
	AuditSourceAgent   = "agent"
	AuditSourceGateway = "gateway"
)

// Reserved / current event types. Unknown types matching auditEventTypeRE are also accepted
// so Host can ship process_open / process_close (and future kinds) without a gateway deploy.
const (
	AuditEventAttempt      = "attempt"
	AuditEventOpened       = "opened"
	AuditEventClosed       = "closed"
	AuditEventFailed       = "failed"
	AuditEventFlag         = "flag"
	AuditEventFileTransfer = "file_transfer"
	AuditEventProcessOpen  = "process_open"  // Host process start (A5b)
	AuditEventProcessClose = "process_close" // Host process exit (A5b)
	AuditEventWindowFocus  = "window_focus"  // Host foreground window (A5c)
	AuditEventWindowTitle  = "window_title"  // Host same-window title change (A5c)
)

var auditEventTypeRE = regexp.MustCompile(`^[a-z][a-z0-9_]{0,63}$`)

type AuditEvent struct {
	ID        int64           `json:"id"`
	SessionID string          `json:"sessionId"`
	At        time.Time       `json:"at"`
	Source    string          `json:"source"`
	Type      string          `json:"type"`
	Detail    json.RawMessage `json:"detail"`
	CreatedAt time.Time       `json:"createdAt"`
}

type AuditEventInsert struct {
	SessionID string
	At        *time.Time
	Source    string
	Type      string
	Detail    json.RawMessage
}

func ValidateAuditEventSource(s string) bool {
	switch s {
	case AuditSourceViewer, AuditSourceAgent, AuditSourceGateway:
		return true
	default:
		return false
	}
}

func NormalizeAuditEventType(t string) (string, error) {
	t = strings.ToLower(strings.TrimSpace(t))
	if !auditEventTypeRE.MatchString(t) {
		return "", fmt.Errorf("无效的 event type: %s", t)
	}
	return t, nil
}

func (s *Store) InsertAuditEvent(ctx context.Context, e AuditEventInsert) (AuditEvent, error) {
	if e.SessionID == "" {
		return AuditEvent{}, errors.New("需要 session id")
	}
	if !ValidateAuditEventSource(e.Source) {
		return AuditEvent{}, fmt.Errorf("无效的 source: %s", e.Source)
	}
	typ, err := NormalizeAuditEventType(e.Type)
	if err != nil {
		return AuditEvent{}, err
	}
	detail := []byte(`{}`)
	if len(e.Detail) > 0 {
		if !json.Valid(e.Detail) {
			return AuditEvent{}, errors.New("detail 须为 JSON 对象")
		}
		detail = e.Detail
	}
	at := time.Now().UTC()
	if e.At != nil {
		at = e.At.UTC()
	}
	row := s.Pool.QueryRow(ctx, `
		INSERT INTO audit_events (session_id, at, source, type, detail)
		VALUES ($1, $2, $3, $4, $5::jsonb)
		RETURNING id, session_id, at, source, type, detail, created_at`,
		e.SessionID, at, e.Source, typ, detail,
	)
	return scanAuditEvent(row)
}

func (s *Store) InsertAuditEvents(ctx context.Context, events []AuditEventInsert) (int, error) {
	if len(events) == 0 {
		return 0, nil
	}
	if len(events) > 200 {
		return 0, errors.New("单次最多 200 条事件")
	}
	tx, err := s.Pool.Begin(ctx)
	if err != nil {
		return 0, err
	}
	defer tx.Rollback(ctx)

	n := 0
	for _, e := range events {
		if e.SessionID == "" {
			return 0, errors.New("需要 session id")
		}
		if !ValidateAuditEventSource(e.Source) {
			return 0, fmt.Errorf("无效的 source: %s", e.Source)
		}
		typ, err := NormalizeAuditEventType(e.Type)
		if err != nil {
			return 0, err
		}
		detail := []byte(`{}`)
		if len(e.Detail) > 0 {
			if !json.Valid(e.Detail) {
				return 0, errors.New("detail 须为 JSON 对象")
			}
			detail = e.Detail
		}
		at := time.Now().UTC()
		if e.At != nil {
			at = e.At.UTC()
		}
		if _, err := tx.Exec(ctx, `
			INSERT INTO audit_events (session_id, at, source, type, detail)
			VALUES ($1, $2, $3, $4, $5::jsonb)`,
			e.SessionID, at, e.Source, typ, detail); err != nil {
			return 0, err
		}
		n++
	}
	if err := tx.Commit(ctx); err != nil {
		return 0, err
	}
	return n, nil
}

func (s *Store) ListAuditEvents(ctx context.Context, sessionID string, limit int) ([]AuditEvent, error) {
	if sessionID == "" {
		return nil, errors.New("需要 session id")
	}
	if limit <= 0 || limit > 1000 {
		limit = 500
	}
	rows, err := s.Pool.Query(ctx, `
		SELECT id, session_id, at, source, type, detail, created_at
		FROM audit_events
		WHERE session_id = $1
		ORDER BY at ASC, id ASC
		LIMIT $2`, sessionID, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []AuditEvent
	for rows.Next() {
		ev, err := scanAuditEvent(rows)
		if err != nil {
			return nil, err
		}
		out = append(out, ev)
	}
	return out, rows.Err()
}

// ListAuditEventsBySessionFilter returns events whose parent session matches the
// same filters as ListAuditSessions (attempt time range, agent, operator, …).
// Used by A5d admin behavior-event CSV export. Cap 20000.
func (s *Store) ListAuditEventsBySessionFilter(ctx context.Context, f AuditListFilter) ([]AuditEvent, error) {
	limit := f.Limit
	if limit <= 0 {
		limit = 5000
	}
	if limit > 20000 {
		limit = 20000
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
	b.WriteString(`
		SELECT e.id, e.session_id, e.at, e.source, e.type, e.detail, e.created_at
		FROM audit_events e
		INNER JOIN audit_sessions a ON a.id = e.session_id
		LEFT JOIN users u ON u.id = a.operator_user_id
		WHERE TRUE`)
	if f.From != nil {
		b.WriteString(` AND a.attempted_at >= ` + arg(f.From.UTC()))
	}
	if f.To != nil {
		b.WriteString(` AND a.attempted_at < ` + arg(f.To.UTC()))
	}
	if f.AgentID != "" {
		b.WriteString(` AND a.agent_id = ` + arg(f.AgentID))
	}
	if f.Operator != "" {
		b.WriteString(` AND a.operator_name ILIKE ` + arg("%"+f.Operator+"%"))
	}
	if f.Result != "" {
		b.WriteString(` AND a.result = ` + arg(f.Result))
	}
	if f.DepartmentID > 0 {
		b.WriteString(` AND u.department_id = ` + arg(f.DepartmentID))
	}
	b.WriteString(` ORDER BY e.at ASC, e.id ASC LIMIT ` + arg(limit))

	rows, err := s.Pool.Query(ctx, b.String(), args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []AuditEvent
	for rows.Next() {
		ev, err := scanAuditEvent(rows)
		if err != nil {
			return nil, err
		}
		out = append(out, ev)
	}
	return out, rows.Err()
}

// PurgeAuditEventsOlderThan deletes behavior events by event time (A5d shorter retention).
func (s *Store) PurgeAuditEventsOlderThan(ctx context.Context, cutoff time.Time) (int64, error) {
	tag, err := s.Pool.Exec(ctx, `DELETE FROM audit_events WHERE at < $1`, cutoff.UTC())
	if err != nil {
		return 0, err
	}
	return tag.RowsAffected(), nil
}

func scanAuditEvent(row pgx.Row) (AuditEvent, error) {
	var e AuditEvent
	var detail []byte
	err := row.Scan(&e.ID, &e.SessionID, &e.At, &e.Source, &e.Type, &detail, &e.CreatedAt)
	if err != nil {
		return e, err
	}
	if len(detail) == 0 {
		e.Detail = json.RawMessage(`{}`)
	} else {
		e.Detail = json.RawMessage(detail)
	}
	return e, nil
}
