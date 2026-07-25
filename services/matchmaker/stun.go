package main

import (
	"encoding/json"
	"errors"
	"log/slog"
	"net"
	"sync"
	"sync/atomic"
	"time"
)

// STUN-style reflexive-address echo (design §2). A client sends StunProbe from
// the SAME UDP socket it will punch/match on; the server replies with the
// source ip:port it observed — that socket's public reflexive address as seen
// from outside the NAT. This is the "learn the address off the wire" trick
// UdpTransport::poll() already does, promoted to a server.
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

const (
	// kStunMaxDatagram bounds an accepted probe. A real probe is ~70 bytes
	// ("seat<N>-<32 hex lobby id>"); this is generous headroom that still stops
	// the reply from being grown by a large echoed nonce.
	kStunMaxDatagram = 512

	// kStunMaxNonceBytes bounds the echoed token itself — the only part of the
	// reply an attacker controls.
	kStunMaxNonceBytes = 128

	// Per-source gate. The C++ StunClient re-probes every 250 ms until it gets
	// an answer, so 20/s sustained with 40 banked is ~5x what several clients
	// behind one NAT produce. Loopback/private sources are exempt.
	kStunIngressSlots  = 4096
	kStunIngressCostMs = 50
	kStunIngressBurst  = 40

	kStunMaintainInterval = 30 * time.Second

	// kUDPMaxConsecutiveErrs stops a genuinely broken socket from spinning the
	// read loop hot, without letting one bad datagram end it (see serve).
	kUDPMaxConsecutiveErrs = 64
)

type stunProbe struct {
	Type  string `json:"type"`
	Nonce string `json:"nonce"`
}

type stunReply struct {
	Type     string `json:"type"`
	Nonce    string `json:"nonce"`
	YourAddr string `json:"your_addr"`
}

// stunCounters is one snapshot of the aggregated tallies. Like the relay, this
// listener never logs per datagram: a log line per hostile packet would itself
// be an amplifier.
type stunCounters struct {
	replied     uint64
	oversize    uint64
	malformed   uint64
	longNonce   uint64
	rateLimited uint64
	readErr     uint64
	writeErr    uint64
}

type stunServer struct {
	conn    *net.UDPConn
	ingress *ipBuckets
	log     *slog.Logger
	done    chan struct{}

	replied     atomic.Uint64
	oversize    atomic.Uint64
	malformed   atomic.Uint64
	longNonce   atomic.Uint64
	rateLimited atomic.Uint64
	readErr     atomic.Uint64
	writeErr    atomic.Uint64

	lastStats stunCounters // maintain goroutine only
	closeOnce sync.Once
}

func startStun(addr string, log *slog.Logger) (*stunServer, error) {
	udpAddr, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		return nil, err
	}
	conn, err := net.ListenUDP("udp", udpAddr)
	if err != nil {
		return nil, err
	}
	s := &stunServer{
		conn:    conn,
		ingress: newIPBuckets(kStunIngressSlots, kStunIngressCostMs, kStunIngressBurst),
		log:     log,
		done:    make(chan struct{}),
	}
	go s.serve()
	go s.maintain()
	return s, nil
}

func (s *stunServer) serve() {
	// One byte of slack so an oversized datagram is DETECTED rather than
	// silently truncated by the kernel copy into an exact-sized buffer.
	buf := make([]byte, kStunMaxDatagram+1)
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
			if errors.Is(err, net.ErrClosed) || errs >= kUDPMaxConsecutiveErrs {
				return
			}
			errs++
			s.readErr.Add(1)
			continue
		}
		errs = 0
		if n > kStunMaxDatagram {
			s.oversize.Add(1)
			continue
		}
		if key := perIPKey(src.String()); key != "" && !s.ingress.allow(key) {
			s.rateLimited.Add(1)
			continue
		}
		var p stunProbe
		if err := json.Unmarshal(buf[:n], &p); err != nil || p.Type != "StunProbe" {
			s.malformed.Add(1)
			continue
		}
		if len(p.Nonce) > kStunMaxNonceBytes {
			s.longNonce.Add(1)
			continue
		}
		reply, err := json.Marshal(stunReply{Type: "StunReply", Nonce: p.Nonce, YourAddr: src.String()})
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

func (s *stunServer) snapshot() stunCounters {
	return stunCounters{
		replied:     s.replied.Load(),
		oversize:    s.oversize.Load(),
		malformed:   s.malformed.Load(),
		longNonce:   s.longNonce.Load(),
		rateLimited: s.rateLimited.Load(),
		readErr:     s.readErr.Load(),
		writeErr:    s.writeErr.Load(),
	}
}

func (s *stunServer) logStats() {
	cur := s.snapshot()
	if cur == s.lastStats {
		return
	}
	s.log.Info("stun stats",
		"replied", cur.replied,
		"drop_oversize", cur.oversize, "drop_malformed", cur.malformed,
		"drop_long_nonce", cur.longNonce, "drop_rate_limited", cur.rateLimited,
		"read_err", cur.readErr, "write_err", cur.writeErr)
	s.lastStats = cur
}

func (s *stunServer) maintain() {
	tk := time.NewTicker(kStunMaintainInterval)
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

func (s *stunServer) LocalAddr() net.Addr { return s.conn.LocalAddr() }

func (s *stunServer) Close() error {
	s.closeOnce.Do(func() { close(s.done) })
	return s.conn.Close()
}
