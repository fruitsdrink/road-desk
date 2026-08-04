package bootstrap

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"log"
	"os"
	"path/filepath"

	"github.com/jackc/pgx/v5/pgxpool"
	"golang.org/x/crypto/bcrypt"
)

const (
	SecretAgentPSK  = "agent_psk"
	SecretViewerPSK = "viewer_psk"
	// Legacy DB key name; migrated to SecretViewerPSK on boot.
	secretDirectoryPSKLegacy = "directory_psk"
	SecretJWT                = "jwt_secret"
)

func Ensure(ctx context.Context, pool *pgxpool.Pool, dataDir string) error {
	if err := migrateDirectoryPSKToViewer(ctx, pool, dataDir); err != nil {
		return err
	}
	if err := ensureSecret(ctx, pool, dataDir, SecretAgentPSK, "agent-control.psk", 32); err != nil {
		return err
	}
	if err := ensureSecret(ctx, pool, dataDir, SecretViewerPSK, "viewer.psk", 32); err != nil {
		return err
	}
	if err := ensureJWTSecret(ctx, pool); err != nil {
		return err
	}
	return ensureAdmin(ctx, pool)
}

func migrateDirectoryPSKToViewer(ctx context.Context, pool *pgxpool.Pool, dataDir string) error {
	var old string
	err := pool.QueryRow(ctx, `SELECT value FROM secrets WHERE name=$1`, secretDirectoryPSKLegacy).Scan(&old)
	if err != nil || old == "" {
		return nil
	}
	var newer string
	err = pool.QueryRow(ctx, `SELECT value FROM secrets WHERE name=$1`, SecretViewerPSK).Scan(&newer)
	if err != nil || newer == "" {
		_, err = pool.Exec(ctx, `
			INSERT INTO secrets(name, value) VALUES ($1,$2)
			ON CONFLICT (name) DO UPDATE SET value=EXCLUDED.value, updated_at=NOW()`,
			SecretViewerPSK, old)
		if err != nil {
			return err
		}
	}
	_, _ = pool.Exec(ctx, `DELETE FROM secrets WHERE name=$1`, secretDirectoryPSKLegacy)
	_ = os.Remove(filepath.Join(dataDir, "directory.psk"))
	log.Printf("migrated secret directory_psk -> viewer_psk (file viewer.psk)")
	return nil
}

func ensureSecret(ctx context.Context, pool *pgxpool.Pool, dataDir, name, filename string, n int) error {
	var existing string
	err := pool.QueryRow(ctx, `SELECT value FROM secrets WHERE name=$1`, name).Scan(&existing)
	if err == nil && existing != "" {
		path := filepath.Join(dataDir, filename)
		_ = os.WriteFile(path, []byte(existing+"\n"), 0o600)
		log.Printf("secret %s already set; file: %s", name, path)
		return nil
	}
	raw := make([]byte, n)
	if _, err := rand.Read(raw); err != nil {
		return err
	}
	value := hex.EncodeToString(raw)
	_, err = pool.Exec(ctx, `
		INSERT INTO secrets(name, value) VALUES ($1,$2)
		ON CONFLICT (name) DO UPDATE SET value=EXCLUDED.value, updated_at=NOW()`, name, value)
	if err != nil {
		return err
	}
	path := filepath.Join(dataDir, filename)
	if err := os.WriteFile(path, []byte(value+"\n"), 0o600); err != nil {
		return err
	}
	log.Printf("generated %s -> %s", name, path)
	return nil
}

func ensureJWTSecret(ctx context.Context, pool *pgxpool.Pool) error {
	var existing string
	err := pool.QueryRow(ctx, `SELECT value FROM secrets WHERE name=$1`, SecretJWT).Scan(&existing)
	if err == nil && existing != "" {
		return nil
	}
	if env := os.Getenv("ROAD_DESK_GATEWAY_JWT_SECRET"); env != "" {
		_, err = pool.Exec(ctx, `
			INSERT INTO secrets(name, value) VALUES ($1,$2)
			ON CONFLICT (name) DO UPDATE SET value=EXCLUDED.value, updated_at=NOW()`, SecretJWT, env)
		return err
	}
	raw := make([]byte, 32)
	if _, err := rand.Read(raw); err != nil {
		return err
	}
	value := hex.EncodeToString(raw)
	_, err = pool.Exec(ctx, `
		INSERT INTO secrets(name, value) VALUES ($1,$2)
		ON CONFLICT (name) DO UPDATE SET value=EXCLUDED.value, updated_at=NOW()`, SecretJWT, value)
	return err
}

func ensureAdmin(ctx context.Context, pool *pgxpool.Pool) error {
	var n int
	if err := pool.QueryRow(ctx, `SELECT COUNT(*) FROM users WHERE role='admin'`).Scan(&n); err != nil {
		return err
	}
	if n > 0 {
		return nil
	}
	var deptID int64
	if err := pool.QueryRow(ctx, `
		SELECT id FROM departments WHERE name='系统管理' LIMIT 1`).Scan(&deptID); err != nil {
		return fmt.Errorf("system department: %w", err)
	}
	pass := randomPassword(16)
	hash, err := bcrypt.GenerateFromPassword([]byte(pass), bcrypt.DefaultCost)
	if err != nil {
		return err
	}
	_, err = pool.Exec(ctx, `
		INSERT INTO users(username, password_hash, role, department_id)
		VALUES ($1,$2,'admin',$3)`, "admin", string(hash), deptID)
	if err != nil {
		return err
	}
	log.Printf("============================================================")
	log.Printf("INITIAL ADMIN CREDENTIALS (save now; shown only once)")
	log.Printf("  username: admin")
	log.Printf("  password: %s", pass)
	log.Printf("============================================================")
	return nil
}

func randomPassword(n int) string {
	const alphabet = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789"
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		return fmt.Sprintf("changeme-%d", n)
	}
	for i := range b {
		b[i] = alphabet[int(b[i])%len(alphabet)]
	}
	return string(b)
}

func GetSecret(ctx context.Context, pool *pgxpool.Pool, name string) (string, error) {
	var v string
	err := pool.QueryRow(ctx, `SELECT value FROM secrets WHERE name=$1`, name).Scan(&v)
	return v, err
}
