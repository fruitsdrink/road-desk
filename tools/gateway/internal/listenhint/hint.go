package listenhint

import (
	"errors"
	"fmt"
	"net"
	"os/exec"
	"runtime"
	"strconv"
	"strings"
	"syscall"
)

// Format returns a user-facing listen error. On address-in-use it includes the
// occupying PID (when discoverable) and a kill command for the current OS.
func Format(addr string, err error) string {
	if err == nil {
		return ""
	}
	if !IsAddrInUse(err) {
		return err.Error()
	}

	port := portOf(addr)
	var b strings.Builder
	if port != "" {
		fmt.Fprintf(&b, "port %s is already in use", port)
	} else {
		b.WriteString("listen address is already in use")
	}
	fmt.Fprintf(&b, " (%v)", err)

	pid, name := findListener(port)
	if pid > 0 {
		fmt.Fprintf(&b, "\n  occupying process: pid=%d", pid)
		if name != "" {
			fmt.Fprintf(&b, " name=%s", name)
		}
		b.WriteString("\n  to free the port, run:")
		if runtime.GOOS == "windows" {
			fmt.Fprintf(&b, "\n    taskkill /PID %d /F", pid)
		} else {
			fmt.Fprintf(&b, "\n    kill %d", pid)
			fmt.Fprintf(&b, "\n    # if it refuses: kill -9 %d", pid)
		}
		return b.String()
	}

	b.WriteString("\n  could not resolve occupying process; try:")
	if runtime.GOOS == "windows" {
		if port != "" {
			fmt.Fprintf(&b, "\n    netstat -ano | findstr :%s", port)
		} else {
			b.WriteString("\n    netstat -ano")
		}
		b.WriteString("\n    taskkill /PID <pid> /F")
	} else {
		if port != "" {
			fmt.Fprintf(&b, "\n    lsof -nP -iTCP:%s -sTCP:LISTEN", port)
		} else {
			b.WriteString("\n    lsof -nP -iTCP -sTCP:LISTEN")
		}
		b.WriteString("\n    kill <pid>")
	}
	return b.String()
}

// IsAddrInUse reports whether err indicates the listen address is already bound.
func IsAddrInUse(err error) bool {
	if err == nil {
		return false
	}
	if errors.Is(err, syscall.EADDRINUSE) {
		return true
	}
	// Windows / wrapped net errors sometimes only expose the message.
	msg := strings.ToLower(err.Error())
	return strings.Contains(msg, "address already in use") ||
		strings.Contains(msg, "only one usage of each socket address") ||
		strings.Contains(msg, "bind: address already in use")
}

func portOf(addr string) string {
	_, port, err := net.SplitHostPort(addr)
	if err != nil {
		return ""
	}
	return port
}

func findListener(port string) (pid int, name string) {
	if port == "" {
		return 0, ""
	}
	if runtime.GOOS == "windows" {
		pid = findPIDWindows(port)
	} else {
		pid = findPIDUnix(port)
	}
	if pid <= 0 {
		return 0, ""
	}
	return pid, processName(pid)
}

func findPIDWindows(port string) int {
	out, err := exec.Command("netstat", "-ano", "-p", "tcp").Output()
	if err != nil {
		return 0
	}
	for _, line := range strings.Split(string(out), "\n") {
		fields := strings.Fields(line)
		if len(fields) < 5 {
			continue
		}
		if !strings.EqualFold(fields[0], "TCP") {
			continue
		}
		if !strings.EqualFold(fields[3], "LISTENING") {
			continue
		}
		if !localPortEquals(fields[1], port) {
			continue
		}
		pid, err := strconv.Atoi(fields[len(fields)-1])
		if err != nil || pid <= 0 {
			continue
		}
		return pid
	}
	return 0
}

func localPortEquals(local, port string) bool {
	hostport := local
	// netstat may print [::]:8743 (ok) or rare unbracketed IPv6; normalize the latter.
	if strings.Count(local, ":") > 1 && !strings.HasPrefix(local, "[") {
		if i := strings.LastIndex(local, ":"); i > 0 {
			hostport = "[" + local[:i] + "]:" + local[i+1:]
		}
	}
	_, p, err := net.SplitHostPort(hostport)
	if err == nil {
		return p == port
	}
	// Fallback: exact ":port" suffix (avoids matching :18743 for port 8743).
	return strings.HasSuffix(local, ":"+port)
}

func findPIDUnix(port string) int {
	if pid := findPIDLsof(port); pid > 0 {
		return pid
	}
	return findPIDSS(port)
}

func findPIDLsof(port string) int {
	out, err := exec.Command("lsof", "-nP", "-t", "-iTCP:"+port, "-sTCP:LISTEN").Output()
	if err != nil {
		return 0
	}
	for _, line := range strings.Split(string(out), "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		pid, err := strconv.Atoi(line)
		if err == nil && pid > 0 {
			return pid
		}
	}
	return 0
}

func findPIDSS(port string) int {
	out, err := exec.Command("ss", "-lptn", "sport = :"+port).Output()
	if err != nil {
		return 0
	}
	// ss prints pid=1234, or users:(("name",pid=1234,fd=...))
	for _, line := range strings.Split(string(out), "\n") {
		if i := strings.Index(line, "pid="); i >= 0 {
			rest := line[i+4:]
			end := 0
			for end < len(rest) && rest[end] >= '0' && rest[end] <= '9' {
				end++
			}
			if end == 0 {
				continue
			}
			pid, err := strconv.Atoi(rest[:end])
			if err == nil && pid > 0 {
				return pid
			}
		}
	}
	return 0
}

func processName(pid int) string {
	if runtime.GOOS == "windows" {
		out, err := exec.Command("tasklist", "/FI", fmt.Sprintf("PID eq %d", pid), "/FO", "CSV", "/NH").Output()
		if err != nil {
			return ""
		}
		line := strings.TrimSpace(string(out))
		if line == "" || strings.HasPrefix(strings.ToLower(line), "info:") {
			return ""
		}
		// "name.exe","1234",...
		if strings.HasPrefix(line, "\"") {
			if end := strings.Index(line[1:], "\""); end >= 0 {
				return line[1 : 1+end]
			}
		}
		fields := strings.Split(line, ",")
		if len(fields) > 0 {
			return strings.Trim(fields[0], "\"")
		}
		return ""
	}
	out, err := exec.Command("ps", "-p", strconv.Itoa(pid), "-o", "comm=").Output()
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(out))
}
