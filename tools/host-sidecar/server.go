package main

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"strconv"
	"strings"
)

type server struct {
	mgr   *agentManager
	token string
}

func newServer(dir, agentName, logName, token string) *server {
	return &server{
		mgr:   newAgentManager(dir, agentName, logName),
		token: token,
	}
}

func (s *server) listenAndServe(addr string) error {
	mux := http.NewServeMux()
	mux.HandleFunc("/status", s.handleStatus)
	mux.HandleFunc("/logs", s.handleLogs)
	mux.HandleFunc("/deploy", s.handleDeploy)
	mux.HandleFunc("/agent", s.handleDeploy) // alias from docs

	return http.ListenAndServe(addr, s.withAuth(mux))
}

func (s *server) withAuth(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if s.token != "" && r.Header.Get("X-Road-Desk-Token") != s.token {
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		next.ServeHTTP(w, r)
	})
}

func (s *server) handleStatus(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	st, err := s.mgr.status()
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, st)
}

func (s *server) handleLogs(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	tail := 200
	if v := r.URL.Query().Get("tail"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 0 {
			http.Error(w, "invalid tail", http.StatusBadRequest)
			return
		}
		tail = n
	}
	body, err := s.mgr.readLogs(tail)
	if err != nil {
		if os.IsNotExist(err) {
			http.Error(w, "log file not found", http.StatusNotFound)
			return
		}
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	_, _ = w.Write(body)
}

func (s *server) handleDeploy(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	// Limit upload size (64 MiB) — scaffold Agent binaries are tiny.
	r.Body = http.MaxBytesReader(w, r.Body, 64<<20)

	tmp, err := os.CreateTemp(s.mgr.dir, "host-agent-upload-*.tmp")
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	tmpPath := tmp.Name()
	defer func() {
		_ = os.Remove(tmpPath)
	}()

	written, err := io.Copy(tmp, r.Body)
	closeErr := tmp.Close()
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	if closeErr != nil {
		http.Error(w, closeErr.Error(), http.StatusInternalServerError)
		return
	}
	if written == 0 {
		http.Error(w, "empty body", http.StatusBadRequest)
		return
	}

	start := true
	if v := r.URL.Query().Get("start"); v != "" {
		start = v == "1" || strings.EqualFold(v, "true")
	}

	if err := s.mgr.deploy(tmpPath, start); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}

	st, err := s.mgr.status()
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, map[string]interface{}{
		"ok":     true,
		"bytes":  written,
		"status": st,
	})
}

func writeJSON(w http.ResponseWriter, v interface{}) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	enc := json.NewEncoder(w)
	enc.SetIndent("", "  ")
	if err := enc.Encode(v); err != nil {
		fmt.Fprintf(os.Stderr, "encode json: %v\n", err)
	}
}
