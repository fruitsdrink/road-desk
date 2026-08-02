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

type Service struct {
	pool      *pgxpool.Pool
	jwtSecret []byte
}

func New(pool *pgxpool.Pool, jwtSecret string) *Service {
	return &Service{pool: pool, jwtSecret: []byte(jwtSecret)}
}

func (s *Service) Login(ctx context.Context, username, password string) (string, error) {
	var hash string
	err := s.pool.QueryRow(ctx, `SELECT password_hash FROM admins WHERE username=$1`, username).Scan(&hash)
	if err != nil {
		return "", errors.New("invalid credentials")
	}
	if bcrypt.CompareHashAndPassword([]byte(hash), []byte(password)) != nil {
		return "", errors.New("invalid credentials")
	}
	tok := jwt.NewWithClaims(jwt.SigningMethodHS256, jwt.MapClaims{
		"sub":  username,
		"role": "admin",
		"exp":  time.Now().Add(24 * time.Hour).Unix(),
		"iat":  time.Now().Unix(),
	})
	return tok.SignedString(s.jwtSecret)
}

func (s *Service) ParseAdmin(r *http.Request) (string, error) {
	h := r.Header.Get("Authorization")
	if !strings.HasPrefix(h, "Bearer ") {
		return "", errors.New("missing token")
	}
	raw := strings.TrimPrefix(h, "Bearer ")
	tok, err := jwt.Parse(raw, func(t *jwt.Token) (any, error) {
		if t.Method != jwt.SigningMethodHS256 {
			return nil, errors.New("bad alg")
		}
		return s.jwtSecret, nil
	})
	if err != nil || !tok.Valid {
		return "", errors.New("invalid token")
	}
	claims, ok := tok.Claims.(jwt.MapClaims)
	if !ok {
		return "", errors.New("invalid token")
	}
	sub, _ := claims["sub"].(string)
	role, _ := claims["role"].(string)
	if sub == "" || role != "admin" {
		return "", errors.New("invalid token")
	}
	return sub, nil
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
