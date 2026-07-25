// Package stun is the STUN-style reflexive-address echo (design §2,
// PROTOCOL.md §2). A client sends StunProbe from the SAME UDP socket it will
// punch/match on; the server replies with the source ip:port it observed — that
// socket's public reflexive address as seen from outside the NAT. This is the
// "learn the address off the wire" trick UdpTransport::poll() already does,
// promoted to a server.
//
// It is entirely self-contained: it holds no lobby state and never talks to any
// other package but ratelimit.
//
// Datagram format is one JSON object per UDP packet (documented in PROTOCOL.md
// so the C++ side matches):
//
//	C→S  {"type":"StunProbe","nonce":"<opaque echo token>"}
//	S→C  {"type":"StunReply","nonce":"<same>","your_addr":"81.2.3.4:52001"}
//
// nonce is an opaque STRING echoed verbatim (no server interpretation, no JSON
// number precision pitfalls). Malformed or non-probe datagrams are ignored.
//
// AMPLIFICATION (SECURITY.md). This is an unauthenticated UDP echo, so it is a
// reflector by construction: an attacker who spoofs a victim's source address
// makes the server send the reply to the victim. What bounds it is that the
// nonce is echoed VERBATIM, so the reply grows in step with the request and the
// attacker cannot choose a better ratio — measured 35 B → 68 B (×1.94) and
// 1234 B → 1267 B (×1.03) against the live deployment. The caps below pin the
// worst case at the small end of that curve, and the per-source gate stops a
// non-spoofing flood. A ~2x reflector is not a useful one (usable reflectors
// run 50–500x), so the residual is accepted rather than fixed.
package stun

import (
	"encoding/json"
	"errors"
	"net"
	"sync"
	"sync/atomic"
	"time"

	"go.uber.org/zap"

	"github.com/egedemirbas/open-bomberman/matchmaker/internal/ratelimit"
)

const (
	// MaxDatagram bounds an accepted probe. A real probe is ~70 bytes
	// ("seat<N>-<32 hex lobby id>"); this is generous headroom that still stops
	// the reply from being grown by a large echoed nonce.
	MaxDatagram = 512

	// MaxNonceBytes bounds the echoed token itself — the only part of the
	// reply an attacker controls.
	MaxNonceBytes = 128

	// Per-source gate. The C++ StunClient re-probes every 250 ms until it gets
	// an answer, so 20/s sustained with 40 banked is ~5x what several clients
	// behind one NAT produce. Loopback/private sources are exempt.
	kIngressSlots  = 4096
	kIngressCostMs = 50
	kIngressBurst  = 40

	kMaintainInterval = 30 * time.Second

	// kMaxConsecutiveReadErrs stops a genuinely broken socket from spinning the
	// read loop hot, without letting one bad datagram end it (see serve).
	kMaxConsecutiveReadErrs = 64
)

type Probe struct {
	Type  string `json:"type"`
	Nonce string `json:"nonce"`
}

type Reply struct {
	Type     string `json:"type"`
	Nonce    string `json:"nonce"`
	YourAddr string `json:"your_addr"`
}

// counters is one snapshot of the aggregated tallies. Like the relay, this
// listener never logs per datagram: a log line per hostile packet would itself
// be an amplifier.
type counters struct {
	replied     uint64
	oversize    uint64
	malformed   uint64
	longNonce   uint64
	rateLimited uint64
	readErr     uint64
	writeErr    uint64
}

type Server struct {
	conn    *net.UDPConn
	ingress *ratelimit.Table
	log     *zap.Logger
	done    chan struct{}

	// alive is the liveness signal the health check reads; serveDone lets a
	// draining caller wait for the read loop instead of racing it. See the same
	// pair in relay.Server — SECURITY.md F6 is the failure they make visible.
	alive     atomic.Bool
	serveDone chan struct{}

	replied     atomic.Uint64
	oversize    atomic.Uint64
	malformed   atomic.Uint64
	longNonce   atomic.Uint64
	rateLimited atomic.Uint64
	readErr     atomic.Uint64
	writeErr    atomic.Uint64

	lastStats counters // maintain goroutine only
	closeOnce sync.Once
}

func Start(addr string, log *zap.Logger) (*Server, error) {
	udpAddr, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		return nil, err
	}
	conn, err := net.ListenUDP("udp", udpAddr)
	if err != nil {
		return nil, err
	}
	s := &Server{
		conn:      conn,
		ingress:   ratelimit.NewTable(kIngressSlots, kIngressCostMs, kIngressBurst),
		log:       log,
		done:      make(chan struct{}),
		serveDone: make(chan struct{}),
	}
	s.alive.Store(true)
	go s.serve()
	go s.maintain()
	return s, nil
}

// Alive reports whether the echo's read loop is still running.
func (s *Server) Alive() bool { return s.alive.Load() }

func (s *Server) serve() {
	defer func() {
		s.alive.Store(false)
		close(s.serveDone)
	}()
	// One byte of slack so an oversized datagram is DETECTED rather than
	// silently truncated by the kernel copy into an exact-sized buffer.
	buf := make([]byte, MaxDatagram+1)
	errs := 0
	for {
		n, src, err := s.conn.ReadFromUDP(buf)
		if err != nil {
			// ONLY a closed socket ends the loop. Every other error here is
			// per-datagram and attacker-triggerable: Windows reports an
			// oversized datagram as WSAEMSGSIZE and an ICMP port-unreachable
			// from an earlier reply as WSAECONNRESET, rather than truncating
			// or ignoring the way Linux does. Returning on those made one
			// datagram enough to take the listener down for good.
			if errors.Is(err, net.ErrClosed) || errs >= kMaxConsecutiveReadErrs {
				return
			}
			errs++
			s.readErr.Add(1)
			continue
		}
		errs = 0
		if n > MaxDatagram {
			s.oversize.Add(1)
			continue
		}
		if key := ratelimit.SourceKey(src.String()); key != "" && !s.ingress.Allow(key) {
			s.rateLimited.Add(1)
			continue
		}
		var p Probe
		if err := json.Unmarshal(buf[:n], &p); err != nil || p.Type != "StunProbe" {
			s.malformed.Add(1)
			continue
		}
		if len(p.Nonce) > MaxNonceBytes {
			s.longNonce.Add(1)
			continue
		}
		reply, err := json.Marshal(Reply{Type: "StunReply", Nonce: p.Nonce, YourAddr: src.String()})
		if err != nil {
			s.malformed.Add(1)
			continue
		}
		if _, err := s.conn.WriteToUDP(reply, src); err != nil {
			s.writeErr.Add(1)
			continue
		}
		s.replied.Add(1)
	}
}

func (s *Server) snapshot() counters {
	return counters{
		replied:     s.replied.Load(),
		oversize:    s.oversize.Load(),
		malformed:   s.malformed.Load(),
		longNonce:   s.longNonce.Load(),
		rateLimited: s.rateLimited.Load(),
		readErr:     s.readErr.Load(),
		writeErr:    s.writeErr.Load(),
	}
}

func (s *Server) logStats() {
	cur := s.snapshot()
	if cur == s.lastStats {
		return
	}
	s.log.Info("stun stats",
		zap.Uint64("replied", cur.replied),
		zap.Uint64("drop_oversize", cur.oversize), zap.Uint64("drop_malformed", cur.malformed),
		zap.Uint64("drop_long_nonce", cur.longNonce), zap.Uint64("drop_rate_limited", cur.rateLimited),
		zap.Uint64("read_err", cur.readErr), zap.Uint64("write_err", cur.writeErr))
	s.lastStats = cur
}

func (s *Server) maintain() {
	tk := time.NewTicker(kMaintainInterval)
	defer tk.Stop()
	for {
		select {
		case <-s.done:
			return
		case <-tk.C:
			s.logStats()
		}
	}
}

func (s *Server) LocalAddr() net.Addr { return s.conn.LocalAddr() }

// Close stops the listener and returns once the read loop has actually exited.
func (s *Server) Close() error {
	s.closeOnce.Do(func() { close(s.done) })
	err := s.conn.Close()
	<-s.serveDone
	return err
}
