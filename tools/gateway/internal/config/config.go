package config

import (
	"bufio"
	"log"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

type Config struct {
	ListenAddr   string
	DatabaseURL  string
	DataDir      string
	JWTSecret    string
	OnlineAfterS int
	WebDir       string
}

func Load() Config {
	loadDotEnv(".env")
	dataDir := env("ROAD_DESK_GATEWAY_DATA", filepath.Join(".", "data"))
	_ = os.MkdirAll(dataDir, 0o755)
	return Config{
		ListenAddr:   env("ROAD_DESK_GATEWAY_LISTEN", ":8743"),
		DatabaseURL:  env("DATABASE_URL", "postgres://roaddesk:roaddesk@127.0.0.1:5433/roaddesk?sslmode=disable"),
		DataDir:      dataDir,
		JWTSecret:    env("ROAD_DESK_GATEWAY_JWT_SECRET", ""),
		OnlineAfterS: envInt("ROAD_DESK_ONLINE_AFTER_SEC", 45),
		WebDir:       env("ROAD_DESK_GATEWAY_WEB", filepath.Join(".", "web", "dist")),
	}
}

// loadDotEnv loads KEY=VALUE lines from path into the process environment.
// Existing OS env vars win (are not overwritten). Missing file is ignored.
func loadDotEnv(path string) {
	f, err := os.Open(path)
	if err != nil {
		return
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	n := 0
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		if strings.HasPrefix(line, "export ") {
			line = strings.TrimSpace(strings.TrimPrefix(line, "export "))
		}
		eq := strings.IndexByte(line, '=')
		if eq <= 0 {
			continue
		}
		key := strings.TrimSpace(line[:eq])
		val := strings.TrimSpace(line[eq+1:])
		if len(val) >= 2 {
			if (val[0] == '"' && val[len(val)-1] == '"') || (val[0] == '\'' && val[len(val)-1] == '\'') {
				val = val[1 : len(val)-1]
			}
		}
		if key == "" {
			continue
		}
		if os.Getenv(key) != "" {
			continue
		}
		_ = os.Setenv(key, val)
		n++
	}
	if n > 0 {
		log.Printf("loaded %d vars from %s (OS env takes precedence)", n, path)
	}
}

func env(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

func envInt(k string, def int) int {
	v := os.Getenv(k)
	if v == "" {
		return def
	}
	n, err := strconv.Atoi(v)
	if err != nil {
		return def
	}
	return n
}
