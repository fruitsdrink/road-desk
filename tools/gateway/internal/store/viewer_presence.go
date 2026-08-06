package store

import (
	"context"
	"log"
	"time"
)

// SweepStaleViewerPresence clears last_seen_at for viewers whose heartbeat
// is older than OnlineAfterS. Used as a gateway-side fallback when the Viewer
// process exits without calling /v1/viewer/offline.
func (s *Store) SweepStaleViewerPresence(ctx context.Context) (int64, error) {
	after := s.OnlineAfterS
	if after <= 0 {
		after = 45
	}
	tag, err := s.Pool.Exec(ctx, `
		UPDATE viewer_presence
		SET last_seen_at = NULL, updated_at = NOW()
		WHERE last_seen_at IS NOT NULL
		  AND last_seen_at < NOW() - ($1 * INTERVAL '1 second')`, after)
	if err != nil {
		return 0, err
	}
	return tag.RowsAffected(), nil
}

// RunViewerPresenceProbe periodically marks stale Viewer presence rows offline.
func RunViewerPresenceProbe(ctx context.Context, st *Store, intervalSec int) {
	if intervalSec <= 0 {
		return
	}
	ticker := time.NewTicker(time.Duration(intervalSec) * time.Second)
	defer ticker.Stop()
	run := func() {
		n, err := st.SweepStaleViewerPresence(ctx)
		if err != nil {
			if ctx.Err() != nil {
				return
			}
			log.Printf("viewer presence probe: %v", err)
			return
		}
		if n > 0 {
			log.Printf("viewer presence probe: marked %d offline (stale heartbeat)", n)
		}
	}
	run()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			run()
		}
	}
}
