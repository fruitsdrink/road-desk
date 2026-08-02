# Road Desk Gateway

Go control-plane gateway + Docker Postgres. Admin SPA is built from `../../admin` into `web/dist`.

See **[docs/gateway.md](../../docs/gateway.md)** for install, keys, and smoke checklist.

```powershell
docker compose up -d
copy .env.example .env   # optional; DATABASE_URL etc.
go run ./cmd/gateway
```

Default listen `:8743`. Database URL default uses host port **5433**.
PSK files: `data/agent-control.psk` (Agent), `data/viewer.psk` (Viewer).
