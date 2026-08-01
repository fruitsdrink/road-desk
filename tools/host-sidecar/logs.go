package main

import (
	"bytes"
	"os"
)

func (m *agentManager) readLogs(tail int) ([]byte, error) {
	data, err := os.ReadFile(m.logPath())
	if err != nil {
		return nil, err
	}
	if tail <= 0 {
		return data, nil
	}
	return tailLines(data, tail), nil
}

func tailLines(data []byte, n int) []byte {
	if n <= 0 || len(data) == 0 {
		return data
	}
	// Count trailing newlines; keep last n lines.
	lines := 0
	i := len(data)
	for i > 0 {
		i--
		if data[i] == '\n' {
			lines++
			if lines > n {
				i++
				break
			}
		}
	}
	if lines <= n {
		return data
	}
	out := data[i:]
	// Drop a leading CR if we sliced mid-CRLF.
	out = bytes.TrimPrefix(out, []byte{'\r'})
	return out
}
