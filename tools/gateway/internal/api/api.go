package api

import (
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/jackc/pgx/v5"
	"golang.org/x/crypto/bcrypt"

	"github.com/fruitsdrink/road-desk/tools/gateway/internal/auth"
	"github.com/fruitsdrink/road-desk/tools/gateway/internal/bootstrap"
	"github.com/fruitsdrink/road-desk/tools/gateway/internal/store"
)

type Server struct {
	Store   *store.Store
	Auth    *auth.Service
	DataDir string
	WebDir  string
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", s.healthz)

	mux.HandleFunc("POST /v1/agents/register", s.agentRegister)
	mux.HandleFunc("POST /v1/agents/heartbeat", s.agentHeartbeat)

	mux.HandleFunc("POST /v1/admin/login", s.adminLogin)
	mux.HandleFunc("POST /v1/viewer/login", s.viewerLogin)

	mux.HandleFunc("GET /v1/admin/groups", s.requireAdmin(s.listGroups))
	mux.HandleFunc("POST /v1/admin/groups", s.requireAdmin(s.createGroup))
	mux.HandleFunc("PATCH /v1/admin/groups/{id}", s.requireAdmin(s.patchGroup))
	mux.HandleFunc("DELETE /v1/admin/groups/{id}", s.requireAdmin(s.deleteGroup))
	mux.HandleFunc("GET /v1/admin/tags", s.requireAdmin(s.listTags))
	mux.HandleFunc("POST /v1/admin/tags", s.requireAdmin(s.createTag))
	mux.HandleFunc("PATCH /v1/admin/tags/{id}", s.requireAdmin(s.patchTag))
	mux.HandleFunc("DELETE /v1/admin/tags/{id}", s.requireAdmin(s.deleteTag))
	mux.HandleFunc("GET /v1/admin/agents", s.requireAdmin(s.listAgents))
	mux.HandleFunc("GET /v1/admin/agents/{id}", s.requireAdmin(s.getAgent))
	mux.HandleFunc("PATCH /v1/admin/agents/{id}", s.requireAdmin(s.patchAgent))
	mux.HandleFunc("DELETE /v1/admin/agents/{id}", s.requireAdmin(s.deleteAgent))
	mux.HandleFunc("GET /v1/admin/secrets/agent-psk", s.requireAdmin(s.downloadAgentPSK))
	mux.HandleFunc("GET /v1/admin/secrets/viewer-psk", s.requireAdmin(s.downloadViewerPSK))
	mux.HandleFunc("GET /v1/admin/secrets/directory-psk", s.requireAdmin(s.downloadViewerPSK))

	mux.HandleFunc("GET /v1/admin/departments", s.requireAdmin(s.listDepartments))
	mux.HandleFunc("POST /v1/admin/departments", s.requireAdmin(s.createDepartment))
	mux.HandleFunc("PATCH /v1/admin/departments/{id}", s.requireAdmin(s.patchDepartment))
	mux.HandleFunc("DELETE /v1/admin/departments/{id}", s.requireAdmin(s.deleteDepartment))
	mux.HandleFunc("GET /v1/admin/users", s.requireAdmin(s.listUsers))
	mux.HandleFunc("POST /v1/admin/users", s.requireAdmin(s.createUser))
	mux.HandleFunc("PATCH /v1/admin/users/{id}", s.requireAdmin(s.patchUser))
	mux.HandleFunc("DELETE /v1/admin/users/{id}", s.requireAdmin(s.deleteUser))

	mux.HandleFunc("POST /v1/audit/sessions/upsert", s.requireAuditIngest(s.auditUpsert))
	mux.HandleFunc("POST /v1/audit/events", s.requireAuditIngest(s.auditIngestEvents))
	mux.HandleFunc("GET /v1/admin/audit/sessions", s.requireAdmin(s.listAuditSessions))
	mux.HandleFunc("GET /v1/admin/audit/sessions/export", s.requireAdmin(s.exportAuditSessionsCSV))
	mux.HandleFunc("GET /v1/admin/audit/events/export", s.requireAdmin(s.exportAuditEventsCSV))
	mux.HandleFunc("GET /v1/admin/audit/sessions/{id}", s.requireAdmin(s.getAuditSession))

	mux.HandleFunc("GET /v1/directory/tree", s.requireViewer(s.directoryTree))
	mux.HandleFunc("GET /v1/directory/agents", s.requireViewer(s.listAgents))

	mux.Handle("/", s.spa())
	return mux
}

func (s *Server) healthz(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

type agentBody struct {
	AgentID       string   `json:"agentId"`
	Hostname      string   `json:"hostname"`
	Version       string   `json:"version"`
	MediaPort     int      `json:"mediaPort"`
	IPv4s         []string `json:"ipv4s"`
	PreferredIPv4 string   `json:"preferredIpv4"`
}

func (s *Server) agentRegister(w http.ResponseWriter, r *http.Request) {
	if !s.Auth.CheckAgentPSK(r.Context(), r) {
		writeErr(w, http.StatusUnauthorized, "未授权")
		return
	}
	var body agentBody
	if err := decodeJSON(r, &body); err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	if body.AgentID == "" || body.MediaPort <= 0 {
		writeErr(w, http.StatusBadRequest, "需要 agentId 与 mediaPort")
		return
	}
	a, err := s.Store.UpsertRegister(r.Context(), body.AgentID, body.Hostname, body.Version, body.MediaPort, body.IPv4s, body.PreferredIPv4)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, a)
}

func (s *Server) agentHeartbeat(w http.ResponseWriter, r *http.Request) {
	if !s.Auth.CheckAgentPSK(r.Context(), r) {
		writeErr(w, http.StatusUnauthorized, "未授权")
		return
	}
	var body agentBody
	if err := decodeJSON(r, &body); err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	if body.AgentID == "" || body.MediaPort <= 0 {
		writeErr(w, http.StatusBadRequest, "需要 agentId 与 mediaPort")
		return
	}
	a, err := s.Store.Heartbeat(r.Context(), body.AgentID, body.Hostname, body.Version, body.MediaPort, body.IPv4s, body.PreferredIPv4)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, a)
}

func (s *Server) adminLogin(w http.ResponseWriter, r *http.Request) {
	s.login(w, r, auth.RoleAdmin)
}

func (s *Server) viewerLogin(w http.ResponseWriter, r *http.Request) {
	s.login(w, r, auth.RoleViewer)
}

func (s *Server) login(w http.ResponseWriter, r *http.Request, requireRole string) {
	var body struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if err := decodeJSON(r, &body); err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	tok, claims, err := s.Auth.Login(r.Context(), body.Username, body.Password, requireRole)
	if err != nil {
		writeErr(w, http.StatusUnauthorized, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"token":        tok,
		"role":         claims.Role,
		"username":     claims.Username,
		"departmentId": claims.DepartmentID,
		"userId":       claims.UserID,
	})
}

type handlerFunc func(http.ResponseWriter, *http.Request)

func (s *Server) requireAdmin(next handlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if _, err := s.Auth.ParseAdmin(r); err != nil {
			writeErr(w, http.StatusUnauthorized, "未授权")
			return
		}
		next(w, r)
	}
}

func (s *Server) requireViewer(next handlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !s.Auth.CheckViewerAccess(r.Context(), r) {
			writeErr(w, http.StatusUnauthorized, "未授权")
			return
		}
		next(w, r)
	}
}

func (s *Server) listGroups(w http.ResponseWriter, r *http.Request) {
	gs, err := s.Store.ListGroups(r.Context())
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, gs)
}

func (s *Server) createGroup(w http.ResponseWriter, r *http.Request) {
	var body struct {
		ParentID  *int64 `json:"parentId"`
		Name      string `json:"name"`
		SortOrder int    `json:"sortOrder"`
	}
	if err := decodeJSON(r, &body); err != nil || strings.TrimSpace(body.Name) == "" {
		writeErr(w, http.StatusBadRequest, "名称不能为空")
		return
	}
	g, err := s.Store.CreateGroup(r.Context(), body.ParentID, strings.TrimSpace(body.Name), body.SortOrder)
	if err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusCreated, g)
}

func (s *Server) patchGroup(w http.ResponseWriter, r *http.Request) {
	id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if err != nil {
		writeErr(w, http.StatusBadRequest, "无效的 ID")
		return
	}
	var body struct {
		ParentID  *int64  `json:"parentId"`
		Name      *string `json:"name"`
		SortOrder *int    `json:"sortOrder"`
	}
	if err := decodeJSON(r, &body); err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	g, err := s.Store.UpdateGroup(r.Context(), id, body.ParentID, body.Name, body.SortOrder)
	if err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, g)
}

func (s *Server) deleteGroup(w http.ResponseWriter, r *http.Request) {
	id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if err != nil {
		writeErr(w, http.StatusBadRequest, "无效的 ID")
		return
	}
	if err := s.Store.DeleteGroup(r.Context(), id); err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) listTags(w http.ResponseWriter, r *http.Request) {
	ts, err := s.Store.ListTags(r.Context())
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, ts)
}

func (s *Server) createTag(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Name string `json:"name"`
	}
	if err := decodeJSON(r, &body); err != nil || strings.TrimSpace(body.Name) == "" {
		writeErr(w, http.StatusBadRequest, "名称不能为空")
		return
	}
	t, err := s.Store.CreateTag(r.Context(), strings.TrimSpace(body.Name))
	if err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusCreated, t)
}

func (s *Server) patchTag(w http.ResponseWriter, r *http.Request) {
	id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if err != nil {
		writeErr(w, http.StatusBadRequest, "无效的 ID")
		return
	}
	var body struct {
		Name string `json:"name"`
	}
	if err := decodeJSON(r, &body); err != nil || strings.TrimSpace(body.Name) == "" {
		writeErr(w, http.StatusBadRequest, "名称不能为空")
		return
	}
	t, err := s.Store.UpdateTag(r.Context(), id, strings.TrimSpace(body.Name))
	if err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, t)
}

func (s *Server) deleteTag(w http.ResponseWriter, r *http.Request) {
	id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if err != nil {
		writeErr(w, http.StatusBadRequest, "无效的 ID")
		return
	}
	if err := s.Store.DeleteTag(r.Context(), id); err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) listAgents(w http.ResponseWriter, r *http.Request) {
	as, err := s.Store.ListAgents(r.Context())
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, as)
}

func (s *Server) getAgent(w http.ResponseWriter, r *http.Request) {
	a, err := s.Store.GetAgent(r.Context(), r.PathValue("id"))
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			writeErr(w, http.StatusNotFound, "未找到")
			return
		}
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, a)
}

func (s *Server) patchAgent(w http.ResponseWriter, r *http.Request) {
	var body struct {
		DisplayName *string  `json:"displayName"`
		GroupID     *int64   `json:"groupId"`
		TagIDs      *[]int64 `json:"tagIds"`
	}
	if err := decodeJSON(r, &body); err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	a, err := s.Store.UpdateAgent(r.Context(), r.PathValue("id"), body.DisplayName, body.GroupID, body.TagIDs)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			writeErr(w, http.StatusNotFound, "未找到")
			return
		}
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, a)
}

func (s *Server) deleteAgent(w http.ResponseWriter, r *http.Request) {
	if err := s.Store.DeleteAgent(r.Context(), r.PathValue("id")); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			writeErr(w, http.StatusNotFound, "未找到")
			return
		}
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) listDepartments(w http.ResponseWriter, r *http.Request) {
	ds, err := s.Store.ListDepartments(r.Context())
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, ds)
}

func (s *Server) createDepartment(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Name      string `json:"name"`
		SortOrder int    `json:"sortOrder"`
	}
	if err := decodeJSON(r, &body); err != nil || strings.TrimSpace(body.Name) == "" {
		writeErr(w, http.StatusBadRequest, "名称不能为空")
		return
	}
	d, err := s.Store.CreateDepartment(r.Context(), strings.TrimSpace(body.Name), body.SortOrder)
	if err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusCreated, d)
}

func (s *Server) patchDepartment(w http.ResponseWriter, r *http.Request) {
	id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if err != nil {
		writeErr(w, http.StatusBadRequest, "无效的 ID")
		return
	}
	var body struct {
		Name      *string `json:"name"`
		SortOrder *int    `json:"sortOrder"`
	}
	if err := decodeJSON(r, &body); err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	if body.Name != nil {
		trimmed := strings.TrimSpace(*body.Name)
		body.Name = &trimmed
		if trimmed == "" {
			writeErr(w, http.StatusBadRequest, "名称不能为空")
			return
		}
	}
	d, err := s.Store.UpdateDepartment(r.Context(), id, body.Name, body.SortOrder)
	if err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, d)
}

func (s *Server) deleteDepartment(w http.ResponseWriter, r *http.Request) {
	id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if err != nil {
		writeErr(w, http.StatusBadRequest, "无效的 ID")
		return
	}
	if err := s.Store.DeleteDepartment(r.Context(), id); err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) listUsers(w http.ResponseWriter, r *http.Request) {
	us, err := s.Store.ListUsers(r.Context())
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, us)
}

func (s *Server) createUser(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Username     string `json:"username"`
		Password     string `json:"password"`
		Role         string `json:"role"`
		DepartmentID int64  `json:"departmentId"`
		Enabled      *bool  `json:"enabled"`
	}
	if err := decodeJSON(r, &body); err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	username := strings.TrimSpace(body.Username)
	if username == "" || body.Password == "" {
		writeErr(w, http.StatusBadRequest, "请填写用户名和口令")
		return
	}
	if body.DepartmentID <= 0 {
		writeErr(w, http.StatusBadRequest, "请选择部门")
		return
	}
	role := strings.TrimSpace(body.Role)
	if role == "" {
		role = auth.RoleViewer
	}
	enabled := true
	if body.Enabled != nil {
		enabled = *body.Enabled
	}
	hash, err := hashPassword(body.Password)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	u, err := s.Store.CreateUser(r.Context(), username, hash, role, body.DepartmentID, enabled)
	if err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusCreated, u)
}

func (s *Server) patchUser(w http.ResponseWriter, r *http.Request) {
	id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if err != nil {
		writeErr(w, http.StatusBadRequest, "无效的 ID")
		return
	}
	var body struct {
		Password     *string `json:"password"`
		Role         *string `json:"role"`
		DepartmentID *int64  `json:"departmentId"`
		Enabled      *bool   `json:"enabled"`
	}
	if err := decodeJSON(r, &body); err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	var hash *string
	if body.Password != nil {
		if *body.Password == "" {
			writeErr(w, http.StatusBadRequest, "口令不能为空")
			return
		}
		h, err := hashPassword(*body.Password)
		if err != nil {
			writeErr(w, http.StatusInternalServerError, err.Error())
			return
		}
		hash = &h
	}
	if body.Role != nil {
		trimmed := strings.TrimSpace(*body.Role)
		body.Role = &trimmed
	}
	u, err := s.Store.UpdateUser(r.Context(), id, hash, body.Role, body.DepartmentID, body.Enabled)
	if err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, u)
}

func (s *Server) deleteUser(w http.ResponseWriter, r *http.Request) {
	id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if err != nil {
		writeErr(w, http.StatusBadRequest, "无效的 ID")
		return
	}
	if err := s.Store.DeleteUser(r.Context(), id); err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func hashPassword(password string) (string, error) {
	b, err := bcrypt.GenerateFromPassword([]byte(password), bcrypt.DefaultCost)
	if err != nil {
		return "", err
	}
	return string(b), nil
}

func (s *Server) downloadAgentPSK(w http.ResponseWriter, r *http.Request) {
	s.downloadSecret(w, r, bootstrap.SecretAgentPSK, "agent-control.psk")
}

func (s *Server) downloadViewerPSK(w http.ResponseWriter, r *http.Request) {
	s.downloadSecret(w, r, bootstrap.SecretViewerPSK, "viewer.psk")
}

func (s *Server) downloadSecret(w http.ResponseWriter, r *http.Request, name, filename string) {
	v, err := bootstrap.GetSecret(r.Context(), s.Store.Pool, name)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Disposition", `attachment; filename="`+filename+`"`)
	_, _ = io.WriteString(w, strings.TrimSpace(v)+"\n")
}

type treeNode struct {
	ID       int64         `json:"id"`
	Name     string        `json:"name"`
	Children []treeNode    `json:"children"`
	Agents   []store.Agent `json:"agents"`
}

func (s *Server) directoryTree(w http.ResponseWriter, r *http.Request) {
	gs, err := s.Store.ListGroups(r.Context())
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	as, err := s.Store.ListAgents(r.Context())
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	byParent := map[int64][]store.Group{}
	var roots []store.Group
	for _, g := range gs {
		if g.ParentID == nil {
			roots = append(roots, g)
		} else {
			byParent[*g.ParentID] = append(byParent[*g.ParentID], g)
		}
	}
	agentsByGroup := map[int64][]store.Agent{}
	for _, a := range as {
		agentsByGroup[a.GroupID] = append(agentsByGroup[a.GroupID], a)
	}
	var build func(g store.Group) treeNode
	build = func(g store.Group) treeNode {
		n := treeNode{ID: g.ID, Name: g.Name, Children: []treeNode{}, Agents: agentsByGroup[g.ID]}
		if n.Agents == nil {
			n.Agents = []store.Agent{}
		}
		for _, c := range byParent[g.ID] {
			n.Children = append(n.Children, build(c))
		}
		return n
	}
	out := make([]treeNode, 0, len(roots))
	for _, g := range roots {
		out = append(out, build(g))
	}
	writeJSON(w, http.StatusOK, out)
}

func (s *Server) spa() http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasPrefix(r.URL.Path, "/v1/") || r.URL.Path == "/healthz" {
			http.NotFound(w, r)
			return
		}
		root := s.WebDir
		if root == "" {
			http.NotFound(w, r)
			return
		}
		path := filepath.Join(root, filepath.Clean("/"+r.URL.Path))
		if !strings.HasPrefix(path, filepath.Clean(root)) {
			http.NotFound(w, r)
			return
		}
		fi, err := os.Stat(path)
		if err == nil && !fi.IsDir() {
			http.ServeFile(w, r, path)
			return
		}
		index := filepath.Join(root, "index.html")
		if _, err := os.Stat(index); err != nil {
			writeJSON(w, http.StatusOK, map[string]string{
				"service": "road-desk-gateway",
				"hint":    "admin UI not built yet; place dist in web/dist",
			})
			return
		}
		http.ServeFile(w, r, index)
	})
}

func decodeJSON(r *http.Request, dst any) error {
	defer r.Body.Close()
	dec := json.NewDecoder(r.Body)
	return dec.Decode(dst)
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

func writeErr(w http.ResponseWriter, code int, msg string) {
	writeJSON(w, code, map[string]string{"error": friendlyErr(msg)})
}

// friendlyErr maps common English / Postgres messages to Chinese UI copy.
func friendlyErr(msg string) string {
	switch {
	case msg == "":
		return "请求失败"
	case strings.Contains(msg, "users_username_key") || strings.Contains(msg, "duplicate key") && strings.Contains(msg, "username"):
		return "用户名已存在"
	case strings.Contains(msg, "departments_name_key") || (strings.Contains(msg, "duplicate key") && strings.Contains(msg, "departments")):
		return "部门名称已存在"
	case strings.Contains(msg, "tags_name_key") || (strings.Contains(msg, "duplicate key") && strings.Contains(msg, "tags")):
		return "标签名称已存在"
	case strings.Contains(msg, "groups_root_name"):
		return "同级分组名称已存在"
	case strings.Contains(msg, "foreign key") && strings.Contains(msg, "department"):
		return "部门不存在"
	default:
		return msg
	}
}
