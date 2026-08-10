package main

import (
	"net/http"
	"os"
	"path/filepath"
	"strings"
)

// Files that must never be deleted by /fs cleanup.
var protectedFiles = map[string]bool{
	"host-agent.exe":   true,
	"host-sidecar.exe": true,
}

// Handles GET /fs?dir=C:\rd (list) and POST /fs?dir=C:\rd&action=clean (delete
// unprotected files). Lab-only; never expose to the public internet.
func (s *server) handleFS(w http.ResponseWriter, r *http.Request) {
	dir := s.mgr.dir
	if q := r.URL.Query().Get("dir"); q != "" {
		dir = q
	}
	abs, err := filepath.Abs(dir)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	// Restrict to the lab working dir to avoid nuking arbitrary paths.
	if !strings.EqualFold(filepath.Clean(abs), filepath.Clean(s.mgr.dir)) {
		http.Error(w, "dir must be the sidecar working dir", http.StatusForbidden)
		return
	}

	switch r.Method {
	case http.MethodGet:
		entries, err := os.ReadDir(abs)
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		type item struct {
			Name  string `json:"name"`
			IsDir bool   `json:"is_dir"`
			Size  int64  `json:"size"`
		}
		items := make([]item, 0, len(entries))
		for _, e := range entries {
			info, _ := e.Info()
			sz := int64(0)
			if info != nil {
				sz = info.Size()
			}
			items = append(items, item{Name: e.Name(), IsDir: e.IsDir(), Size: sz})
		}
		writeJSON(w, map[string]interface{}{"dir": abs, "items": items})

	case http.MethodPost:
		action := r.URL.Query().Get("action")
		if action != "clean" {
			http.Error(w, "action=clean required", http.StatusBadRequest)
			return
		}
		entries, err := os.ReadDir(abs)
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		deleted := []string{}
		kept := []string{}
		for _, e := range entries {
			if e.IsDir() {
				// Only empty subdirs are safe to remove; keep anything else.
				if r, err := os.ReadDir(filepath.Join(abs, e.Name())); err == nil && len(r) == 0 {
					_ = os.Remove(filepath.Join(abs, e.Name()))
					deleted = append(deleted, e.Name()+"/")
				} else {
					kept = append(kept, e.Name()+"/")
				}
				continue
			}
			if protectedFiles[e.Name()] {
				kept = append(kept, e.Name())
				continue
			}
			if err := os.Remove(filepath.Join(abs, e.Name())); err != nil {
				kept = append(kept, e.Name()+" (locked)")
			} else {
				deleted = append(deleted, e.Name())
			}
		}
		writeJSON(w, map[string]interface{}{
			"dir":     abs,
			"deleted": deleted,
			"kept":    kept,
		})

	default:
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}
