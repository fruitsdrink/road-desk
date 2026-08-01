module github.com/fruitsdrink/road-desk/tools/host-sidecar

// Pin to Go 1.20.x for Win7 / Server 2008 / 2012 Hosts.
// Build with: PATH to go1.20.14, then `go build -o host-sidecar.exe .`
// Do NOT build Win7 Sidecar binaries with Go 1.21+.
go 1.20
