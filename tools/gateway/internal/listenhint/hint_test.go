package listenhint

import (
	"errors"
	"fmt"
	"runtime"
	"strings"
	"syscall"
	"testing"
)

func TestIsAddrInUse(t *testing.T) {
	if !IsAddrInUse(syscall.EADDRINUSE) {
		t.Fatal("EADDRINUSE should match")
	}
	if !IsAddrInUse(fmt.Errorf("listen tcp :8743: bind: address already in use")) {
		t.Fatal("unix-style message should match")
	}
	if !IsAddrInUse(errors.New("listen tcp :8743: bind: Only one usage of each socket address (protocol/network address/port) is normally permitted.")) {
		t.Fatal("windows-style message should match")
	}
	if IsAddrInUse(errors.New("connection refused")) {
		t.Fatal("unrelated error should not match")
	}
}

func TestFormatAddrInUseIncludesKillHint(t *testing.T) {
	msg := Format(":8743", syscall.EADDRINUSE)
	if !strings.Contains(msg, "already in use") {
		t.Fatalf("expected in-use text, got: %s", msg)
	}
	if runtime.GOOS == "windows" {
		if !strings.Contains(msg, "taskkill") {
			t.Fatalf("expected taskkill hint, got: %s", msg)
		}
	} else if !strings.Contains(msg, "kill") {
		t.Fatalf("expected kill hint, got: %s", msg)
	}
}

func TestLocalPortEquals(t *testing.T) {
	cases := []struct {
		local string
		port  string
		want  bool
	}{
		{"0.0.0.0:8743", "8743", true},
		{"127.0.0.1:8743", "8743", true},
		{"[::]:8743", "8743", true},
		{"0.0.0.0:18743", "8743", false},
		{"0.0.0.0:8743", "8744", false},
	}
	for _, tc := range cases {
		if got := localPortEquals(tc.local, tc.port); got != tc.want {
			t.Errorf("localPortEquals(%q, %q)=%v want %v", tc.local, tc.port, got, tc.want)
		}
	}
}
