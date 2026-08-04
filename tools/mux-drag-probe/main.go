// mux-drag-probe: minimal Road-Desk mux client used to stress the host's
// inject + video pipeline (drag simulation) and to capture desktop recon
// screenshots, without depending on the local machine's Schannel stack.
//
// Wire framing (see src/media/mux_protocol.h):
//   u8 channel | u32 le payload_len | payload
package main

import (
	"bytes"
	"compress/zlib"
	"crypto/sha256"
	"crypto/tls"
	"encoding/binary"
	"encoding/hex"
	"flag"
	"fmt"
	"image"
	"image/color"
	"image/jpeg"
	"image/png"
	"io"
	"math"
	"net"
	"os"
	"path/filepath"
	"sync"
	"time"
)

const (
	chControl   = 1
	chInput     = 2
	chVideo     = 3
	chCursor    = 4

	ctrlAuth    = 1
	ctrlAuthOk  = 2
	ctrlAuthFail = 3

	inputPointer = 1

	videoHdrSize = 1 + 4 + 2*4

	codecRawBgra = 1
	codecZlibBgra = 2
	codecCopyRect = 3
	codecJpeg    = 4
	codecH264    = 5

	maxPayload = 16 * 1024 * 1024
)

type Conn struct {
	nc   net.Conn
	r    *bytes.Reader // unused placeholder
	buf  []byte
	rdr  io.Reader
}

func newConn(nc net.Conn) *Conn {
	return &Conn{nc: nc, rdr: nc}
}

func (c *Conn) writeFrame(ch byte, payload []byte) error {
	hdr := make([]byte, 5)
	hdr[0] = ch
	binary.LittleEndian.PutUint32(hdr[1:], uint32(len(payload)))
	if _, err := c.nc.Write(hdr); err != nil {
		return err
	}
	if len(payload) > 0 {
		if _, err := c.nc.Write(payload); err != nil {
			return err
		}
	}
	return nil
}

func (c *Conn) readFrame() (byte, []byte, error) {
	hdr := make([]byte, 5)
	if _, err := io.ReadFull(c.rdr, hdr); err != nil {
		return 0, nil, err
	}
	ch := hdr[0]
	n := binary.LittleEndian.Uint32(hdr[1:])
	if n > maxPayload {
		return 0, nil, fmt.Errorf("payload too large: %d", n)
	}
	payload := make([]byte, n)
	if _, err := io.ReadFull(c.rdr, payload); err != nil {
		return 0, nil, err
	}
	return ch, payload, nil
}

func (c *Conn) close() { c.nc.Close() }

type FrameStats struct {
	mu        sync.Mutex
	msgs      int
	ids       map[uint32]struct{}
	minID     uint32
	maxID     uint32
	haveID    bool
	videoBytes int64
	cursorMsgs int
	fullDesk  []byte // raw BGRA buffer when captured
	fullW     int
	fullH     int
	jpegs     []SavedJpeg
}

type SavedJpeg struct {
	id    uint32
	x, y  int
	w, h  int
	bytes []byte
}

func (fs *FrameStats) recordVideo(payload []byte) {
	if len(payload) < videoHdrSize {
		return
	}
	codec := payload[0]
	id := binary.LittleEndian.Uint32(payload[1:5])
	x := int(binary.LittleEndian.Uint16(payload[5:7]))
	y := int(binary.LittleEndian.Uint16(payload[7:9]))
	w := int(binary.LittleEndian.Uint16(payload[9:11]))
	h := int(binary.LittleEndian.Uint16(payload[11:13]))
	fs.mu.Lock()
	defer fs.mu.Unlock()
	fs.msgs++
	fs.videoBytes += int64(len(payload))
	if !fs.haveID || id < fs.minID {
		fs.minID = id
	}
	if !fs.haveID || id > fs.maxID {
		fs.maxID = id
	}
	fs.haveID = true
	fs.ids[id] = struct{}{}
	_ = codec
	_ = x
	_ = y
	_ = w
	_ = h
}

type PerSecond struct {
	sec    int
	msgs   int
	ids    int
	gaps   int
	bytes  int64
	cursor int
}

type Observer struct {
	fs     *FrameStats
	closed chan struct{}
	last   time.Time
	cur    *PerSecond
	all    []PerSecond
	stop   bool
	armed  bool
	deskW, deskH int

	mouseX, mouseY int
	snapMs  int64
	full    []byte
	lastSnap time.Time
	snapIdx int
	t0      time.Time
	csv     *os.File
	outDir  string
	composed bool
}

func (o *Observer) tick() {
	now := time.Now()
	if o.cur == nil {
		o.cur = &PerSecond{}
		o.last = now
	}
	o.fs.mu.Lock()
	o.cur.msgs = o.fs.msgs
	o.cur.ids = len(o.fs.ids)
	if o.fs.haveID {
		span := int(o.fs.maxID - o.fs.minID + 1)
		if span < o.cur.ids {
			span = o.cur.ids
		}
		o.cur.gaps = span - o.cur.ids
	}
	o.cur.bytes = o.fs.videoBytes
	o.cur.cursor = o.fs.cursorMsgs
	o.fs.mu.Unlock()
}

func (o *Observer) print(sec int, tag string) {
	o.tick()
	cur := o.cur
	_ = sec
	fmt.Printf("[%02d] %s msgs=%-6d ids=%-6d gaps=%-5d bytes=%-10d cursor=%d\n",
		sec, tag, cur.msgs, cur.ids, cur.gaps, cur.bytes, cur.cursor)
	o.all = append(o.all, *cur)
	o.fs.mu.Lock()
	o.fs.msgs = 0
	o.fs.ids = map[uint32]struct{}{}
	o.fs.haveID = false
	o.fs.videoBytes = 0
	o.fs.cursorMsgs = 0
	o.fs.mu.Unlock()
	o.cur = &PerSecond{}
	o.last = time.Now()
}

func (o *Observer) reader(c *Conn, outDir string, wantShot bool) error {
	for {
		ch, payload, err := c.readFrame()
		if err != nil {
			select {
			case <-o.closed:
				return nil
			default:
			}
			return err
		}
		switch ch {
		case chVideo:
			if len(payload) < videoHdrSize {
				continue
			}
			if verbose {
				codec := payload[0]
				id := binary.LittleEndian.Uint32(payload[1:5])
				x := int(binary.LittleEndian.Uint16(payload[5:7]))
				y := int(binary.LittleEndian.Uint16(payload[7:9]))
				w := int(binary.LittleEndian.Uint16(payload[9:11]))
				h := int(binary.LittleEndian.Uint16(payload[11:13]))
				fmt.Printf("  video id=%-6d codec=%d rect=%d,%d %dx%d bytes=%d\n", id, codec, x, y, w, h, len(payload))
			}
			o.fs.recordVideo(payload)
			if !wantShot && o.snapMs > 0 {
				codec := payload[0]
				id := binary.LittleEndian.Uint32(payload[1:5])
				x := int(binary.LittleEndian.Uint16(payload[5:7]))
				y := int(binary.LittleEndian.Uint16(payload[7:9]))
				w := int(binary.LittleEndian.Uint16(payload[9:11]))
				h := int(binary.LittleEndian.Uint16(payload[11:13]))
				o.composeAndSnap(codec, payload, x, y, w, h, id)
			}
			if wantShot && o.armed {
				codec := payload[0]
				id := binary.LittleEndian.Uint32(payload[1:5])
				x := int(binary.LittleEndian.Uint16(payload[5:7]))
				y := int(binary.LittleEndian.Uint16(payload[7:9]))
				w := int(binary.LittleEndian.Uint16(payload[9:11]))
				h := int(binary.LittleEndian.Uint16(payload[11:13]))
				if codec == codecJpeg && w >= o.deskW*9/10 && h >= o.deskH*9/10 {
					_ = saveJpeg(filepath.Join(outDir, fmt.Sprintf("shot_%06d_%dx%d.jpg", id, w, h)), payload[videoHdrSize:])
					o.stop = true
					return nil
				}
				if codec == codecZlibBgra || codec == codecRawBgra {
					rawAll := payload[videoHdrSize:]
					if codec == codecZlibBgra {
						zr, err := zlib.NewReader(bytes.NewReader(rawAll))
						if err == nil {
							rawAll = make([]byte, w*h*4)
							_, _ = io.ReadFull(zr, rawAll)
							zr.Close()
						}
					}
					_ = os.WriteFile(filepath.Join(outDir, fmt.Sprintf("raw_%06d_%dx%d.bgra", id, w, h)), rawAll, 0o644)
					// accumulate into full buffer
					o.fs.mu.Lock()
					if o.fs.fullDesk == nil {
						o.fs.fullDesk = make([]byte, o.deskW*o.deskH*4)
						o.fs.fullW = o.deskW
						o.fs.fullH = o.deskH
					}
					data := payload[videoHdrSize:]
					if codec == codecZlibBgra {
						zr, err := zlib.NewReader(bytes.NewReader(data))
						if err == nil {
							raw := make([]byte, w*h*4)
							_, _ = io.ReadFull(zr, raw)
							zr.Close()
							copyRowBlock(o.fs.fullDesk, o.deskW, raw, x, y, w, h)
						}
					} else {
						copyRowBlock(o.fs.fullDesk, o.deskW, data, x, y, w, h)
					}
					full := o.fs.fullDesk
					fsW, fsH := o.fs.fullW, o.fs.fullH
					o.fs.mu.Unlock()
					// crude: save when rect covers the whole desk (skip all-black frames —
					// VMware DDA can feed black before the host falls back to GDI)
					if w >= fsW*9/10 && h >= fsH*9/10 && !fullFrameBlack(full, fsW, fsH) {
						_ = saveBgraPng(filepath.Join(outDir, fmt.Sprintf("shot_%06d_%dx%d.png", id, w, h)), full, fsW, fsH)
						o.stop = true
						return nil
					}
				}
			}
		case chCursor:
			o.fs.mu.Lock()
			o.fs.cursorMsgs++
			o.fs.mu.Unlock()
		}
	}
}

func fullFrameBlack(bgra []byte, w, h int) bool {
	if w <= 0 || h <= 0 || len(bgra) < w*h*4 {
		return false
	}
	step := 1
	if n := w * h; n > 65536 {
		step = n / 65536
	}
	nonblack := 0
	sampled := 0
	for i := 0; i < w*h; i += step {
		p := bgra[i*4 : i*4+4]
		if p[0] > 16 || p[1] > 16 || p[2] > 16 {
			nonblack++
		}
		sampled++
	}
	return sampled == 0 || nonblack*200 < sampled
}

func copyRowBlock(dst []byte, dstW int, src []byte, x, y, w, h int) {
	if dstW <= 0 || x < 0 || y < 0 || w <= 0 || h <= 0 {
		return
	}
	for row := 0; row < h; row++ {
		si := row * w * 4
		di := ((y+row)*dstW + x) * 4
		if di+ (w*4) > len(dst) {
			continue
		}
		copy(dst[di:di+w*4], src[si:si+w*4])
	}
}


func (o *Observer) composeAndSnap(codec byte, payload []byte, x, y, w, h int, id uint32) {
	if o.snapMs <= 0 {
		return
	}
	o.fs.mu.Lock()
	defer o.fs.mu.Unlock()
	if o.full == nil {
		o.full = make([]byte, o.deskW*o.deskH*4)
	}
	data := payload[videoHdrSize:]
	switch codec {
	case codecJpeg:
		img, err := jpeg.Decode(bytes.NewReader(data))
		if err != nil || img.Bounds().Dx() != w || img.Bounds().Dy() != h {
			return
		}
		rgba, ok := img.(*image.RGBA)
		if !ok {
			rgba2 := image.NewRGBA(img.Bounds())
			drawRGBA(rgba2, img)
			rgba = rgba2
		}
		buf := make([]byte, w*h*4)
		for row := 0; row < h; row++ {
			for col := 0; col < w; col++ {
				i := (row*w + col) * 4
				r, g, b, a := rgba.Pix[i], rgba.Pix[i+1], rgba.Pix[i+2], rgba.Pix[i+3]
				buf[i] = b
				buf[i+1] = g
				buf[i+2] = r
				buf[i+3] = a
			}
		}
		copyRowBlock(o.full, o.deskW, buf, x, y, w, h)
	case codecZlibBgra:
		zr, err := zlib.NewReader(bytes.NewReader(data))
		if err != nil {
			return
		}
		raw := make([]byte, w*h*4)
		_, _ = io.ReadFull(zr, raw)
		zr.Close()
		copyRowBlock(o.full, o.deskW, raw, x, y, w, h)
	case codecRawBgra:
		copyRowBlock(o.full, o.deskW, data, x, y, w, h)
	case codecCopyRect:
		sx := int(binary.LittleEndian.Uint16(payload[13:15]))
		sy := int(binary.LittleEndian.Uint16(payload[15:17]))
		for row := 0; row < h; row++ {
			si := ((sy+row)*o.deskW + sx) * 4
			di := ((y+row)*o.deskW + x) * 4
			if si < 0 || di < 0 || si+(w*4) > len(o.full) || di+(w*4) > len(o.full) {
				continue
			}
			copy(o.full[di:di+w*4], o.full[si:si+w*4])
		}
	default:
		return
	}
	o.composed = true
	now := time.Now()
	if o.lastSnap.IsZero() {
		o.lastSnap = now
		return
	}
	if now.Sub(o.lastSnap) < time.Duration(o.snapMs)*time.Millisecond {
		return
	}
	o.lastSnap = now
	o.snapIdx++
	name := fmt.Sprintf("snap_%04d_id%06d_%dx%d.png", o.snapIdx, id, o.deskW, o.deskH)
	img := image.NewRGBA(image.Rect(0, 0, o.deskW, o.deskH))
	for i := 0; i < len(o.full); i += 4 {
		b, g, r, a := o.full[i], o.full[i+1], o.full[i+2], o.full[i+3]
		img.Pix[i] = r
		img.Pix[i+1] = g
		img.Pix[i+2] = b
		img.Pix[i+3] = a
	}
	// mouse marker: red cross
	mx, my := o.mouseX, o.mouseY
	for dy := -9; dy <= 9; dy++ {
		for dx := -9; dx <= 9; dx++ {
			if dx*dx+dy*dy > 49 {
				continue
			}
			px, py := mx+dx, my+dy
			if px < 0 || py < 0 || px >= o.deskW || py >= o.deskH {
				continue
			}
			img.Set(px, py, color.RGBA{255, 40, 40, 255})
		}
	}
	dir := o.outDir
	if o.csv != nil {
		dir = filepath.Dir(o.csv.Name())
	}
	snapPath := filepath.Join(dir, name)
	f, err := os.Create(snapPath)
	if err == nil {
		_ = png.Encode(f, img)
		f.Close()
	}
	if o.csv != nil {
		fmt.Fprintf(o.csv, "%d,%d,%d,%d\n", o.snapIdx, int(now.Sub(o.t0).Milliseconds()), mx, my)
		o.csv.Sync()
	}
}

func drawRGBA(dst *image.RGBA, src image.Image) {
	b := dst.Bounds()
	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			r, g, bl, a := src.At(x, y).RGBA()
			i := (y*b.Max.X + x) * 4
			dst.Pix[i] = uint8(r >> 8)
			dst.Pix[i+1] = uint8(g >> 8)
			dst.Pix[i+2] = uint8(bl >> 8)
			dst.Pix[i+3] = uint8(a >> 8)
		}
	}
}

func saveBgraPng(path string, bgra []byte, w, h int) error {
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	for i := 0; i < len(bgra); i += 4 {
		b, g, r, a := bgra[i], bgra[i+1], bgra[i+2], bgra[i+3]
		img.Pix[i] = r
		img.Pix[i+1] = g
		img.Pix[i+2] = b
		img.Pix[i+3] = a
	}
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	return png.Encode(f, img)
}

func saveJpeg(path string, data []byte) error {
	return os.WriteFile(path, data, 0o644)
}

func checkFingerprint(der []byte, expectedHex string) error {
	sum := sha256.Sum256(der)
	got := hex.EncodeToString(sum[:])
	if !stringsEqualFold(got, expectedHex) {
		return fmt.Errorf("fingerprint mismatch: got %s want %s", got, expectedHex)
	}
	return nil
}

func stringsEqualFold(a, b string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := 0; i < len(a); i++ {
		ca, cb := a[i], b[i]
		if 'A' <= ca && ca <= 'Z' {
			ca += 32
		}
		if 'A' <= cb && cb <= 'Z' {
			cb += 32
		}
		if ca != cb {
			return false
		}
	}
	return true
}

func dial(host string, port int, expectedFP string) (*Conn, int, int, error) {
	addr := fmt.Sprintf("%s:%d", host, port)
	cfg := &tls.Config{
		InsecureSkipVerify: true, // fingerprint checked below
		MinVersion:         tls.VersionTLS10,
		MaxVersion:         tls.VersionTLS12,
	}
	nc, err := tls.Dial("tcp", addr, cfg)
	if err != nil {
		return nil, 0, 0, err
	}
	state := nc.ConnectionState()
	if expectedFP != "" {
		if len(state.PeerCertificates) == 0 {
			nc.Close()
			return nil, 0, 0, fmt.Errorf("no peer cert")
		}
		if err := checkFingerprint(state.PeerCertificates[0].Raw, expectedFP); err != nil {
			nc.Close()
			return nil, 0, 0, err
		}
	}
	c := newConn(nc)
	// auth
	body := make([]byte, 3+len(password))
	body[0] = ctrlAuth
	binary.LittleEndian.PutUint16(body[1:], uint16(len(password)))
	copy(body[3:], []byte(password))
	if err := c.writeFrame(chControl, body); err != nil {
		c.close()
		return nil, 0, 0, err
	}
	deadline := time.Now().Add(8 * time.Second)
	nc.SetReadDeadline(deadline)
	ch, payload, err := c.readFrame()
	nc.SetReadDeadline(time.Time{})
	if err != nil {
		c.close()
		return nil, 0, 0, fmt.Errorf("auth read: %v", err)
	}
	if ch != chControl || len(payload) < 5 {
		c.close()
		return nil, 0, 0, fmt.Errorf("unexpected auth reply ch=%d len=%d", ch, len(payload))
	}
	switch payload[0] {
	case ctrlAuthFail:
		c.close()
		return nil, 0, 0, fmt.Errorf("auth rejected")
	case ctrlAuthOk:
		w := int(binary.LittleEndian.Uint16(payload[1:3]))
		h := int(binary.LittleEndian.Uint16(payload[3:5]))
		return c, w, h, nil
	default:
		c.close()
		return nil, 0, 0, fmt.Errorf("unexpected control type %d", payload[0])
	}
}

var password string
var verbose bool

func pointerPayload(buttons int, x, y int) []byte {
	p := make([]byte, 6)
	p[0] = inputPointer
	p[1] = byte(buttons & 0xff)
	binary.LittleEndian.PutUint16(p[2:], uint16(x))
	binary.LittleEndian.PutUint16(p[4:], uint16(y))
	return p
}

func runDrag(c *Conn, deskW, deskH int, hz float64, x0, y0, x1, y1 int, secs float64, obs *Observer) {
	interval := time.Duration(float64(time.Second) / hz)
	t0 := time.Now()
	end := t0.Add(time.Duration(secs * float64(time.Second)))
	// LMB down at start
	c.writeFrame(chInput, pointerPayload(1, x0, y0))
	n := 0
	for time.Now().Before(end) {
		frac := float64(n%1000) / 1000.0
		// sweep between corners along a slightly curved path (covers more screen)
		px := x0 + int(float64(x1-x0)*frac)
		py := y0 + int(float64(y1-y0)*math.Sin(frac*math.Pi))
		obs.mouseX = px
		obs.mouseY = py
		if px < 0 {
			px = 0
		}
		if py < 0 {
			py = 0
		}
		if px >= deskW {
			px = deskW - 1
		}
		if py >= deskH {
			py = deskH - 1
		}
		if err := c.writeFrame(chInput, pointerPayload(1, px, py)); err != nil {
			fmt.Println("drag write failed:", err)
			break
		}
		n++
		time.Sleep(interval)
	}
	// LMB up
	c.writeFrame(chInput, pointerPayload(0, x1, y1))
	fmt.Printf("drag: sent %d pointer events over %.1fs (%.0f hz)\n", n, time.Since(t0).Seconds(), hz)
}

func main() {
	var host string
	var port int
	var mode string
	var secs float64
	var hz float64
	var outDir string
	var fp string
	var x0, y0, x1, y1 int
	var shotDelay float64
	var snapMs int64

	flag.StringVar(&host, "host", "", "VM IP")
	flag.IntVar(&port, "port", 38471, "mux TLS port")
	flag.StringVar(&mode, "mode", "shot", "shot|idle|drag")
	flag.Float64Var(&secs, "secs", 12, "duration of drag in seconds")
	flag.Float64Var(&hz, "hz", 200, "pointer event rate during drag")
	flag.Float64Var(&shotDelay, "shot-delay", 0, "seconds to wait before saving frames in shot mode")
	flag.StringVar(&outDir, "out", ".", "output dir for screenshots")
	flag.StringVar(&fp, "fp", "39a0462cd62e85319ef4d768f88a12988bfe5f76f53c125d178b21fc7f1c4086", "expected cert fingerprint (hex)")
	flag.Int64Var(&snapMs, "snap-ms", 300, "save full-frame snapshot every N ms during drag")
	flag.IntVar(&x0, "x0", 0, "drag start x")
	flag.IntVar(&y0, "y0", 0, "drag start y")
	flag.IntVar(&x1, "x1", 0, "drag end x")
	flag.IntVar(&y1, "y1", 0, "drag end y")
	flag.StringVar(&password, "pass", "road-desk", "auth password")
	flag.BoolVar(&verbose, "verbose", false, "log each video message")
	flag.Parse()

	if host == "" {
		fmt.Fprintln(os.Stderr, "usage: mux-drag-probe -host <ip> [-mode shot|idle|drag] ...")
		os.Exit(2)
	}
	_ = os.MkdirAll(outDir, 0o755)

	c, deskW, deskH, err := dial(host, port, fp)
	if err != nil {
		fmt.Fprintln(os.Stderr, "dial failed:", err)
		os.Exit(1)
	}
	defer c.close()
	fmt.Printf("connected desktop=%dx%d mode=%s\n", deskW, deskH, mode)

	fs := &FrameStats{ids: map[uint32]struct{}{}}
	obs := &Observer{fs: fs, closed: make(chan struct{}), deskW: deskW, deskH: deskH,
		snapMs: snapMs, t0: time.Now(), outDir: outDir}
	if snapMs > 0 && mode == "drag" {
		if f, err := os.Create(filepath.Join(outDir, "snap.csv")); err == nil {
			obs.csv = f
			fmt.Fprintln(f, "idx,ms,mx,my")
		}
	}
	go obs.reader(c, outDir, mode == "shot")

	if mode == "shot" {
		if shotDelay > 0 {
			time.Sleep(time.Duration(shotDelay * float64(time.Second)))
		}
		obs.armed = true
		deadline := time.Now().Add(8 * time.Second)
		for time.Now().Before(deadline) {
			if obs.stop {
				break
			}
			time.Sleep(100 * time.Millisecond)
		}
		fmt.Println("shot done")
		return
	}

	// idle phase
	for s := 0; s < 2; s++ {
		time.Sleep(time.Second)
		obs.print(s, "idle")
	}

	if mode == "drag" {
		if x1 == 0 && y1 == 0 {
			// default sweep across the middle of the screen
			x0 = deskW / 4
			y0 = deskH / 2
			x1 = deskW * 3 / 4
			y1 = deskH / 2
		}
		runDrag(c, deskW, deskH, hz, x0, y0, x1, y1, secs, obs)
	}

	// post drag idle
	for s := 0; s < 2; s++ {
		time.Sleep(time.Second)
		obs.print(2+s, "post")
	}

	close(obs.closed)
	fs.mu.Lock()
	fmt.Printf("TOTAL msgs=%d max_id=%d video_bytes=%d\n", fs.msgs, fs.maxID, fs.videoBytes)
	fs.mu.Unlock()
}
