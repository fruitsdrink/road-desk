package main

import (
	"flag"
	"fmt"
	"log"
	"os"
	"path/filepath"
)

func main() {
	addr := flag.String("addr", ":17890", "HTTP listen address (lab LAN only)")
	dir := flag.String("dir", "", "directory for Agent binary and logs (default: sidecar executable dir)")
	agentName := flag.String("agent", "host-agent.exe", "Agent executable file name")
	logName := flag.String("log", "host-agent.log", "Agent log file name")
	token := flag.String("token", "", "optional shared lab token (X-Road-Desk-Token)")
	flag.Parse()

	workDir := *dir
	if workDir == "" {
		exe, err := os.Executable()
		if err != nil {
			log.Fatalf("resolve executable: %v", err)
		}
		workDir = filepath.Dir(exe)
	}
	abs, err := filepath.Abs(workDir)
	if err != nil {
		log.Fatalf("resolve dir: %v", err)
	}
	if err := os.MkdirAll(abs, 0o755); err != nil {
		log.Fatalf("create dir: %v", err)
	}

	s := newServer(abs, *agentName, *logName, *token)
	fmt.Printf("host-sidecar listening on %s\n", *addr)
	fmt.Printf("  dir=%s agent=%s log=%s\n", abs, *agentName, *logName)
	fmt.Printf("  POST /deploy  GET /logs  GET /status\n")
	if err := s.listenAndServe(*addr); err != nil {
		log.Fatal(err)
	}
}
