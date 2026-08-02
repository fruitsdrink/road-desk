package main

import (
	"context"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/fruitsdrink/road-desk/tools/gateway/internal/api"
	"github.com/fruitsdrink/road-desk/tools/gateway/internal/auth"
	"github.com/fruitsdrink/road-desk/tools/gateway/internal/bootstrap"
	"github.com/fruitsdrink/road-desk/tools/gateway/internal/config"
	"github.com/fruitsdrink/road-desk/tools/gateway/internal/db"
	"github.com/fruitsdrink/road-desk/tools/gateway/internal/store"
)

func main() {
	cfg := config.Load()
	ctx := context.Background()

	pool, err := db.Connect(ctx, cfg.DatabaseURL)
	if err != nil {
		log.Fatalf("database: %v", err)
	}
	defer pool.Close()

	if err := db.Migrate(ctx, pool); err != nil {
		log.Fatalf("migrate: %v", err)
	}
	if err := bootstrap.Ensure(ctx, pool, cfg.DataDir); err != nil {
		log.Fatalf("bootstrap: %v", err)
	}

	jwtSecret, err := bootstrap.GetSecret(ctx, pool, bootstrap.SecretJWT)
	if err != nil {
		log.Fatalf("jwt secret: %v", err)
	}
	if cfg.JWTSecret != "" {
		jwtSecret = cfg.JWTSecret
	}

	srv := &api.Server{
		Store:   &store.Store{Pool: pool, OnlineAfterS: cfg.OnlineAfterS},
		Auth:    auth.New(pool, jwtSecret),
		DataDir: cfg.DataDir,
		WebDir:  cfg.WebDir,
	}

	httpSrv := &http.Server{
		Addr:              cfg.ListenAddr,
		Handler:           srv.Handler(),
		ReadHeaderTimeout: 10 * time.Second,
	}

	go func() {
		log.Printf("road-desk gateway listening on %s", cfg.ListenAddr)
		if err := httpSrv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Fatalf("listen: %v", err)
		}
	}()

	ch := make(chan os.Signal, 1)
	signal.Notify(ch, os.Interrupt, syscall.SIGTERM)
	<-ch
	shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	_ = httpSrv.Shutdown(shutdownCtx)
}
