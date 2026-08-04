package auth

import (
	"context"
	"crypto/subtle"
	"errors"
	"net/http"
	"strings"
	"time"

	"github.com/golang-jwt/jwt/v5"
	"github.com/jackc/pgx/v5/pgxpool"
	"golang.org/x/crypto/bcrypt"

	"github.com/fruitsdrink/road-desk/tools/gateway/internal/bootstrap"
)

const (
	RoleAdmin  = "admin"
	RoleViewer = "viewer"
)

type Service struct {
	pool      *pgxpool.Pool
	jwtSecret []byte
}

func New(pool *pgxpool.Pool, jwtSecret string) *Service {
	return &Service{pool: pool, jwtSecret: []byte(jwtSecret)}
}

type Claims struct {
	Username     string
	Role         string
	DepartmentID int64
	UserID       int64
}

func (s *Service) Login(ctx context.Context, username, password, requireRole string) (string, Claims, error) {
	var c Claims
	var hash string
	var enabled bool
	err := s.pool.QueryRow(ctx, `
		SELECT id, username, password_hash, role, department_id, enabled
		FROM users WHERE username=$1`, username).
		Scan(&c.UserID, &c.Username, &hash, &c.Role, &c.DepartmentID, &enabled)
	if err != nil {
		return "", Claims{}, errors.New("用户名或密码错误")
	}
	if !enabled {
		return "", Claims{}, errors.New("账号已停用")
	}
	if bcrypt.CompareHashAndPassword([]byte(hash), []byte(password)) != nil {
		return "", Claims{}, errors.New("用户名或密码错误")
	}
	// Viewer console: admin and operator (viewer) both allowed.
	if requireRole == RoleViewer {
		if c.Role != RoleViewer && c.Role != RoleAdmin {
			return "", Claims{}, errors.New("该账号无权使用 Viewer")
		}
	} else if requireRole != "" && c.Role != requireRole {
		if requireRole == RoleAdmin {
			return "", Claims{}, errors.New("该账号不是管理员，请使用 Viewer 登录")
		}
		return "", Claims{}, errors.New("用户名或密码错误")
	}
	tok := jwt.NewWithClaims(jwt.SigningMethodHS256, jwt.MapClaims{
		"sub":  c.Username,
		"uid":  c.UserID,
		"role": c.Role,
		"dep":  c.DepartmentID,
		"exp":  time.Now().Add(24 * time.Hour).Unix(),
		"iat":  time.Now().Unix(),
	})
	signed, err := tok.SignedString(s.jwtSecret)
	if err != nil {
		return "", Claims{}, err
	}
	return signed, c, nil
}

func (s *Service) ParseToken(r *http.Request) (Claims, error) {
	h := r.Header.Get("Authorization")
	if !strings.HasPrefix(h, "Bearer ") {
		return Claims{}, errors.New("missing token")
	}
	raw := strings.TrimPrefix(h, "Bearer ")
	tok, err := jwt.Parse(raw, func(t *jwt.Token) (any, error) {
		if t.Method != jwt.SigningMethodHS256 {
			return nil, errors.New("bad alg")
		}
		return s.jwtSecret, nil
	})
	if err != nil || !tok.Valid {
		return Claims{}, errors.New("invalid token")
	}
	claims, ok := tok.Claims.(jwt.MapClaims)
	if !ok {
		return Claims{}, errors.New("invalid token")
	}
	sub, _ := claims["sub"].(string)
	role, _ := claims["role"].(string)
	if sub == "" || (role != RoleAdmin && role != RoleViewer) {
		return Claims{}, errors.New("invalid token")
	}
	c := Claims{Username: sub, Role: role}
	switch v := claims["uid"].(type) {
	case float64:
		c.UserID = int64(v)
	}
	switch v := claims["dep"].(type) {
	case float64:
		c.DepartmentID = int64(v)
	}
	return c, nil
}

func (s *Service) ParseAdmin(r *http.Request) (string, error) {
	c, err := s.ParseToken(r)
	if err != nil {
		return "", err
	}
	if c.Role != RoleAdmin {
		return "", errors.New("invalid token")
	}
	return c.Username, nil
}

func (s *Service) CheckAgentPSK(ctx context.Context, r *http.Request) bool {
	want, err := bootstrap.GetSecret(ctx, s.pool, bootstrap.SecretAgentPSK)
	if err != nil {
		return false
	}
	got := bearerOrHeader(r, "X-Road-Desk-Agent-Key")
	return subtle.ConstantTimeCompare([]byte(got), []byte(strings.TrimSpace(want))) == 1
}

func (s *Service) CheckViewerPSK(ctx context.Context, r *http.Request) bool {
	want, err := bootstrap.GetSecret(ctx, s.pool, bootstrap.SecretViewerPSK)
	if err != nil {
		return false
	}
	got := bearerOrHeader(r, "X-Road-Desk-Viewer-Key", "X-Road-Desk-Directory-Key")
	return subtle.ConstantTimeCompare([]byte(got), []byte(strings.TrimSpace(want))) == 1
}

// CheckViewerAccess accepts viewer.psk or a valid JWT (viewer or admin role).
func (s *Service) CheckViewerAccess(ctx context.Context, r *http.Request) bool {
	if s.CheckViewerPSK(ctx, r) {
		return true
	}
	c, err := s.ParseToken(r)
	if err != nil {
		return false
	}
	return c.Role == RoleViewer || c.Role == RoleAdmin
}

func bearerOrHeader(r *http.Request, alts ...string) string {
	h := r.Header.Get("Authorization")
	if strings.HasPrefix(h, "Bearer ") {
		return strings.TrimSpace(strings.TrimPrefix(h, "Bearer "))
	}
	for _, alt := range alts {
		if v := strings.TrimSpace(r.Header.Get(alt)); v != "" {
			return v
		}
	}
	return ""
}
