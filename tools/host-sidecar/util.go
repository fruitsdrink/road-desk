package main

import (
	"strconv"
	"strings"
)

func containsFold(s, substr string) bool {
	return strings.Contains(strings.ToLower(s), strings.ToLower(substr))
}

func trimSpace(s string) string {
	return strings.TrimSpace(s)
}

func itoa(n int) string {
	return strconv.Itoa(n)
}
