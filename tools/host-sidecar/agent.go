package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"syscall"
	"time"
)

type agentStatus struct {
	SidecarOK    bool   `json:"sidecar_ok"`
	AgentRunning bool   `json:"agent_running"`
	AgentPath    string `json:"agent_path"`
	AgentExists  bool   `json:"agent_exists"`
	AgentSize    int64  `json:"agent_size,omitempty"`
	AgentMTime   string `json:"agent_mtime,omitempty"`
	AgentPID     int    `json:"agent_pid,omitempty"`
	Dir          string `json:"dir"`
}

type agentManager struct {
	dir       string
	agentName string
	logName   string

	mu      sync.Mutex
	cmd     *exec.Cmd
	started bool
}

func newAgentManager(dir, agentName, logName string) *agentManager {
	return &agentManager{
		dir:       dir,
		agentName: agentName,
		logName:   logName,
	}
}

func (m *agentManager) agentPath() string {
	return filepath.Join(m.dir, m.agentName)
}

func (m *agentManager) logPath() string {
	return filepath.Join(m.dir, m.logName)
}

func (m *agentManager) status() (agentStatus, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	st := agentStatus{
		SidecarOK: true,
		Dir:       m.dir,
		AgentPath: m.agentPath(),
	}

	if fi, err := os.Stat(m.agentPath()); err == nil {
		st.AgentExists = true
		st.AgentSize = fi.Size()
		st.AgentMTime = fi.ModTime().UTC().Format(time.RFC3339)
	} else if !os.IsNotExist(err) {
		return st, err
	}

	running, pid := m.isRunningLocked()
	st.AgentRunning = running
	st.AgentPID = pid
	return st, nil
}

func (m *agentManager) deploy(tmpPath string, start bool) error {
	m.mu.Lock()
	defer m.mu.Unlock()

	if err := m.stopLocked(); err != nil {
		return fmt.Errorf("stop agent: %w", err)
	}

	dest := m.agentPath()
	if err := os.Remove(dest); err != nil && !os.IsNotExist(err) {
		// On Windows, a still-locked file can fail; surface it clearly.
		return fmt.Errorf("remove old agent: %w", err)
	}
	if err := os.Rename(tmpPath, dest); err != nil {
		// Rename across volumes can fail; fall back to copy.
		if err := copyFile(tmpPath, dest); err != nil {
			return fmt.Errorf("install agent: %w", err)
		}
		_ = os.Remove(tmpPath)
	}

	if start {
		if err := m.startLocked(); err != nil {
			return fmt.Errorf("start agent: %w", err)
		}
	}
	return nil
}

func (m *agentManager) startLocked() error {
	if running, _ := m.isRunningLocked(); running {
		return nil
	}
	path := m.agentPath()
	if _, err := os.Stat(path); err != nil {
		return err
	}
	cmd := exec.Command(path)
	cmd.Dir = m.dir
	cmd.Stdout = nil
	cmd.Stderr = nil
	// Detach from Sidecar console job so Agent can outlive brief Sidecar hiccups
	// during lab use; Sidecar still tracks the child for stop/status when possible.
	cmd.SysProcAttr = &syscall.SysProcAttr{
		CreationFlags: syscall.CREATE_NEW_PROCESS_GROUP,
	}
	if err := cmd.Start(); err != nil {
		return err
	}
	m.cmd = cmd
	m.started = true
	go func(c *exec.Cmd) {
		_ = c.Wait()
		m.mu.Lock()
		defer m.mu.Unlock()
		if m.cmd == c {
			m.cmd = nil
			m.started = false
		}
	}(cmd)
	return nil
}

func (m *agentManager) stopLocked() error {
	if m.cmd != nil && m.cmd.Process != nil {
		_ = m.cmd.Process.Kill()
		// Wait briefly via the Wait goroutine; also fall through to image kill.
		m.cmd = nil
		m.started = false
	}
	// Always try by image name so manually started Agents are replaced too.
	if err := killByImageName(m.agentName); err != nil {
		return err
	}
	// Give Windows a moment to release the executable file lock.
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if !processImageRunning(m.agentName) {
			return nil
		}
		time.Sleep(100 * time.Millisecond)
	}
	if processImageRunning(m.agentName) {
		return errors.New("agent process still running after kill")
	}
	return nil
}

func (m *agentManager) isRunningLocked() (bool, int) {
	if m.cmd != nil && m.cmd.Process != nil {
		// If we started it, prefer our tracked PID when still alive.
		if processAlive(m.cmd.Process.Pid) {
			return true, m.cmd.Process.Pid
		}
	}
	if processImageRunning(m.agentName) {
		return true, 0
	}
	return false, 0
}

func killByImageName(image string) error {
	// taskkill is available on Win7+. /F force, /IM image name.
	cmd := exec.Command("taskkill", "/F", "/IM", image)
	out, err := cmd.CombinedOutput()
	if err == nil {
		return nil
	}
	msg := string(out)
	// "not found" is success for our purposes.
	// Chinese Windows emits GBK ("没有找到进程"); Go may see mojibake — also treat
	// taskkill exit 128 as "image not running" (documented on Win7+).
	if containsFold(msg, "not found") || containsFold(msg, "没有找到") {
		return nil
	}
	var ee *exec.ExitError
	if errors.As(err, &ee) && ee.ExitCode() == 128 {
		return nil
	}
	// If the image is already gone, ignore locale-specific taskkill text.
	if !processImageRunning(image) {
		return nil
	}
	return fmt.Errorf("taskkill %s: %v (%s)", image, err, trimSpace(msg))
}

func processImageRunning(image string) bool {
	cmd := exec.Command("tasklist", "/FI", "IMAGENAME eq "+image, "/NH")
	out, err := cmd.CombinedOutput()
	if err != nil {
		return false
	}
	return containsFold(string(out), image)
}

func processAlive(pid int) bool {
	if pid <= 0 {
		return false
	}
	cmd := exec.Command("tasklist", "/FI", "PID eq "+itoa(pid), "/NH")
	out, err := cmd.CombinedOutput()
	if err != nil {
		return false
	}
	return containsFold(string(out), itoa(pid))
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(dst, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o755)
	if err != nil {
		return err
	}
	_, copyErr := io.Copy(out, in)
	closeErr := out.Close()
	if copyErr != nil {
		return copyErr
	}
	return closeErr
}
