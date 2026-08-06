package main

import (
	"context"
	"log"
	"net"
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
	"github.com/fruitsdrink/road-desk/tools/gateway/internal/listenhint"
	"github.com/fruitsdrink/road-desk/tools/gateway/internal/store"
)

func main() {
	cfg := config.Load()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

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

	st := &store.Store{Pool: pool, OnlineAfterS: cfg.OnlineAfterS}
	srv := &api.Server{
		Store:   st,
		Auth:    auth.New(pool, jwtSecret),
		DataDir: cfg.DataDir,
		WebDir:  cfg.WebDir,
	}

	ln, err := net.Listen("tcp", cfg.ListenAddr)
	if err != nil {
		log.Fatalf("listen %s: %s", cfg.ListenAddr, listenhint.Format(cfg.ListenAddr, err))
	}

	httpSrv := &http.Server{
		Addr:              cfg.ListenAddr,
		Handler:           srv.Handler(),
		ReadHeaderTimeout: 10 * time.Second,
	}

	go func() {
		log.Printf("road-desk gateway listening on %s", cfg.ListenAddr)
		if err := httpSrv.Serve(ln); err != nil && err != http.ErrServerClosed {
			log.Fatalf("serve: %v", err)
		}
	}()

	if cfg.AuditRetentionDays > 0 || cfg.AuditEventsRetentionDays > 0 {
		log.Printf("audit retention: sessions=%d days events=%d days (0 disables that purge)",
			cfg.AuditRetentionDays, cfg.AuditEventsRetentionDays)
		go store.RunAuditRetention(ctx, st, cfg.AuditRetentionDays, cfg.AuditEventsRetentionDays)
	} else {
		log.Printf("audit retention: disabled")
	}

	if cfg.ViewerProbeIntervalS > 0 {
		log.Printf("viewer presence probe: every %ds (offline after %ds without heartbeat)",
			cfg.ViewerProbeIntervalS, cfg.OnlineAfterS)
		go store.RunViewerPresenceProbe(ctx, st, cfg.ViewerProbeIntervalS)
	} else {
		log.Printf("viewer presence probe: disabled")
	}

	ch := make(chan os.Signal, 1)
	signal.Notify(ch, os.Interrupt, syscall.SIGTERM)
	<-ch
	cancel()
	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer shutdownCancel()
	_ = httpSrv.Shutdown(shutdownCtx)
}
