package api

import (
	"encoding/csv"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"

	"github.com/fruitsdrink/road-desk/tools/gateway/internal/store"
)

var uuidRE = regexp.MustCompile(`(?i)^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$`)

type auditUpsertBody struct {
	ID               string          `json:"id"`
	Source           string          `json:"source"` // viewer | agent (informational)
	Phase            string          `json:"phase"`
	OperatorUserID   *int64          `json:"operatorUserId"`
	OperatorName     *string         `json:"operatorName"`
	ViewerHost       *string         `json:"viewerHost"`
	ViewerIP         *string         `json:"viewerIp"`
	AgentID          *string         `json:"agentId"`
	AgentName        *string         `json:"agentName"`
	AgentEndpoint    *string         `json:"agentEndpoint"`
	Mode             *string         `json:"mode"`
	Result           *string         `json:"result"`
	DisconnectReason *string         `json:"disconnectReason"`
	UsedClipboard    *bool           `json:"usedClipboard"`
	UsedFileTransfer *bool           `json:"usedFileTransfer"`
	AttemptedAt      *time.Time      `json:"attemptedAt"`
	OpenedAt         *time.Time      `json:"openedAt"`
	ClosedAt         *time.Time      `json:"closedAt"`
	Partial          *bool           `json:"partial"`
	Meta             json.RawMessage `json:"meta"`
}

func (s *Server) requireAuditIngest(next handlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if s.Auth.CheckAgentPSK(r.Context(), r) || s.Auth.CheckViewerAccess(r.Context(), r) {
			next(w, r)
			return
		}
		writeErr(w, http.StatusUnauthorized, "未授权")
	}
}

func (s *Server) auditUpsert(w http.ResponseWriter, r *http.Request) {
	var body auditUpsertBody
	if err := decodeJSON(r, &body); err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	id := strings.TrimSpace(body.ID)
	if !uuidRE.MatchString(id) {
		writeErr(w, http.StatusBadRequest, "id 须为 UUID")
		return
	}

	u := store.AuditUpsert{
		ID:               strings.ToLower(id),
		OperatorUserID:   body.OperatorUserID,
		OperatorName:     body.OperatorName,
		ViewerHost:       body.ViewerHost,
		ViewerIP:         body.ViewerIP,
		AgentID:          body.AgentID,
		AgentName:        body.AgentName,
		AgentEndpoint:    body.AgentEndpoint,
		Mode:             body.Mode,
		Result:           body.Result,
		DisconnectReason: body.DisconnectReason,
		UsedClipboard:    body.UsedClipboard,
		UsedFileTransfer: body.UsedFileTransfer,
		AttemptedAt:      body.AttemptedAt,
		OpenedAt:         body.OpenedAt,
		ClosedAt:         body.ClosedAt,
		Partial:          body.Partial,
		Meta:             body.Meta,
	}

	// Viewer JWT: fill operator identity when omitted.
	if !s.Auth.CheckAgentPSK(r.Context(), r) {
		if c, err := s.Auth.ParseToken(r); err == nil {
			if u.OperatorUserID == nil && c.UserID > 0 {
				uid := c.UserID
				u.OperatorUserID = &uid
			}
			if (u.OperatorName == nil || strings.TrimSpace(*u.OperatorName) == "") && c.Username != "" {
				name := c.Username
				u.OperatorName = &name
			}
		} else if s.Auth.CheckViewerPSK(r.Context(), r) {
			if u.OperatorName == nil || strings.TrimSpace(*u.OperatorName) == "" {
				name := "viewer_psk"
				u.OperatorName = &name
			}
		}
	}

	// Default attempted_at for attempt/opened if missing.
	phase := strings.ToLower(strings.TrimSpace(body.Phase))
	now := time.Now().UTC()
	if u.AttemptedAt == nil && (phase == "" || phase == "attempt" || phase == "opened" || phase == "failed") {
		u.AttemptedAt = &now
	}
	if phase == "opened" && u.OpenedAt == nil {
		u.OpenedAt = &now
		if u.Result == nil {
			ok := "ok"
			u.Result = &ok
		}
	}
	if phase == "closed" && u.ClosedAt == nil {
		u.ClosedAt = &now
	}
	if phase == "failed" && u.Result == nil {
		unk := "unknown"
		u.Result = &unk
	}

	a, err := s.Store.UpsertAuditSession(r.Context(), u)
	if err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, a)
}

func (s *Server) parseAuditListFilter(r *http.Request, export bool) (store.AuditListFilter, error) {
	q := r.URL.Query()
	f := store.AuditListFilter{
		AgentID:  strings.TrimSpace(q.Get("agent_id")),
		Operator: strings.TrimSpace(q.Get("operator")),
		Result:   strings.TrimSpace(q.Get("result")),
	}
	if v := strings.TrimSpace(q.Get("department_id")); v != "" {
		n, err := strconv.ParseInt(v, 10, 64)
		if err != nil || n < 1 {
			return f, fmt.Errorf("department_id 无效")
		}
		f.DepartmentID = n
	}
	if v := q.Get("limit"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 1 {
			return f, fmt.Errorf("limit 无效")
		}
		f.Limit = n
	} else if export {
		f.Limit = 5000
	}
	if v := q.Get("from"); v != "" {
		t, err := time.Parse(time.RFC3339, v)
		if err != nil {
			return f, fmt.Errorf("from 须为 RFC3339")
		}
		f.From = &t
	}
	if v := q.Get("to"); v != "" {
		t, err := time.Parse(time.RFC3339, v)
		if err != nil {
			return f, fmt.Errorf("to 须为 RFC3339")
		}
		f.To = &t
	}
	if cur := strings.TrimSpace(q.Get("cursor")); cur != "" {
		if export {
			return f, fmt.Errorf("导出不支持 cursor")
		}
		parts := strings.SplitN(cur, "|", 2)
		if len(parts) != 2 {
			return f, fmt.Errorf("cursor 格式为 attemptedAt|id")
		}
		t, err := time.Parse(time.RFC3339Nano, parts[0])
		if err != nil {
			t, err = time.Parse(time.RFC3339, parts[0])
		}
		if err != nil || !uuidRE.MatchString(parts[1]) {
			return f, fmt.Errorf("cursor 格式为 attemptedAt|id")
		}
		f.CursorAt = &t
		f.CursorID = strings.ToLower(parts[1])
	}
	if f.Result != "" && !store.ValidateAuditResult(f.Result) {
		return f, fmt.Errorf("result 无效")
	}
	if !export {
		if f.Limit <= 0 || f.Limit > 200 {
			f.Limit = 50
		}
	}
	return f, nil
}

func (s *Server) listAuditSessions(w http.ResponseWriter, r *http.Request) {
	f, err := s.parseAuditListFilter(r, false)
	if err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}

	items, err := s.Store.ListAuditSessions(r.Context(), f)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	out := map[string]any{"items": items}
	if len(items) == f.Limit {
		last := items[len(items)-1]
		out["nextCursor"] = last.AttemptedAt.UTC().Format(time.RFC3339Nano) + "|" + last.ID
	}
	writeJSON(w, http.StatusOK, out)
}

func csvCell(s string) string {
	return strings.ReplaceAll(s, "\r\n", " ")
}

func (s *Server) exportAuditSessionsCSV(w http.ResponseWriter, r *http.Request) {
	f, err := s.parseAuditListFilter(r, true)
	if err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}

	items, err := s.Store.ListAuditSessions(r.Context(), f)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}

	filename := "audit-sessions-" + time.Now().UTC().Format("20060102-150405") + ".csv"
	w.Header().Set("Content-Type", "text/csv; charset=utf-8")
	w.Header().Set("Content-Disposition", `attachment; filename="`+filename+`"`)
	// UTF-8 BOM so Excel opens Chinese correctly.
	_, _ = w.Write([]byte{0xEF, 0xBB, 0xBF})
	cw := csv.NewWriter(w)
	_ = cw.Write([]string{
		"id", "attemptedAt", "openedAt", "closedAt", "operator", "department",
		"viewerHost", "viewerIp", "agentId", "agentName", "agentEndpoint",
		"mode", "result", "disconnectReason", "usedClipboard", "usedFileTransfer",
		"partial", "meta",
	})
	for _, a := range items {
		opened, closed := "", ""
		if a.OpenedAt != nil {
			opened = a.OpenedAt.UTC().Format(time.RFC3339)
		}
		if a.ClosedAt != nil {
			closed = a.ClosedAt.UTC().Format(time.RFC3339)
		}
		meta := string(a.Meta)
		if meta == "" {
			meta = "{}"
		}
		_ = cw.Write([]string{
			a.ID,
			a.AttemptedAt.UTC().Format(time.RFC3339),
			opened,
			closed,
			csvCell(a.OperatorName),
			csvCell(a.DepartmentName),
			csvCell(a.ViewerHost),
			csvCell(a.ViewerIP),
			csvCell(a.AgentID),
			csvCell(a.AgentName),
			csvCell(a.AgentEndpoint),
			a.Mode,
			a.Result,
			csvCell(a.DisconnectReason),
			strconv.FormatBool(a.UsedClipboard),
			strconv.FormatBool(a.UsedFileTransfer),
			strconv.FormatBool(a.Partial),
			csvCell(meta),
		})
	}
	cw.Flush()
	if err := cw.Error(); err != nil {
		// Headers may already be sent.
		return
	}
}

func (s *Server) getAuditSession(w http.ResponseWriter, r *http.Request) {
	id := strings.TrimSpace(r.PathValue("id"))
	if !uuidRE.MatchString(id) {
		writeErr(w, http.StatusBadRequest, "id 须为 UUID")
		return
	}
	a, err := s.Store.GetAuditSession(r.Context(), strings.ToLower(id))
	if errors.Is(err, pgx.ErrNoRows) {
		writeErr(w, http.StatusNotFound, "未找到")
		return
	}
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, a)
}