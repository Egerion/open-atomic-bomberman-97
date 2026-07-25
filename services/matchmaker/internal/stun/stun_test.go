package stun

import (
	"encoding/json"
	"net"
	"strings"
	"testing"
	"time"

	"github.com/egedemirbas/open-bomberman/matchmaker/internal/config"
)

func TestStunEcho(t *testing.T) {
	s, err := Start("127.0.0.1:0", config.NewLogger("error"))
	if err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer func() { _ = s.Close() }()

	conn, err := net.DialUDP("udp", nil, s.LocalAddr().(*net.UDPAddr))
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer func() { _ = conn.Close() }()

	probe, _ := json.Marshal(Probe{Type: "StunProbe", Nonce: "abc-123"})
	if _, err := conn.Write(probe); err != nil {
		t.Fatalf("write probe: %v", err)
	}

	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 2048)
	n, err := conn.Read(buf)
	if err != nil {
		t.Fatalf("read reply: %v", err)
	}
	var reply Reply
	if err := json.Unmarshal(buf[:n], &reply); err != nil {
		t.Fatalf("decode reply: %v", err)
	}
	if reply.Type != "StunReply" {
		t.Fatalf("reply type %q, want StunReply", reply.Type)
	}
	if reply.Nonce != "abc-123" {
		t.Fatalf("nonce not echoed: %q", reply.Nonce)
	}
	// your_addr must be the source the server observed = this socket's local addr.
	if reply.YourAddr != conn.LocalAddr().String() {
		t.Fatalf("your_addr %q, want %q", reply.YourAddr, conn.LocalAddr().String())
	}
}

func TestStunIgnoresGarbage(t *testing.T) {
	s, err := Start("127.0.0.1:0", config.NewLogger("error"))
	if err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer func() { _ = s.Close() }()

	conn, err := net.DialUDP("udp", nil, s.LocalAddr().(*net.UDPAddr))
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer func() { _ = conn.Close() }()

	// Non-probe payload → no reply.
	if _, err := conn.Write([]byte("not json at all")); err != nil {
		t.Fatalf("write: %v", err)
	}
	_ = conn.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	buf := make([]byte, 256)
	if _, err := conn.Read(buf); err == nil {
		t.Fatal("server should not reply to garbage")
	}
}

// ---- UDP listener survival ----------------------------------------------------

// One datagram must not be able to kill a UDP listener. On Linux the kernel
// truncates an oversized datagram silently, but on Windows ReadFromUDP returns
// WSAEMSGSIZE — and the read loops used to `return` on any error, so a single
// 3 KB packet ended the relay (or an ICMP port-unreachable from a peer that had
// simply quit, which needs no attacker at all).
func TestStunStillAnswersAfterAnOversizedDatagram(t *testing.T) {
	s, err := Start("127.0.0.1:0", config.NewLogger("error"))
	if err != nil {
		t.Fatalf("startStun: %v", err)
	}
	defer func() { _ = s.Close() }()

	conn, err := net.DialUDP("udp", nil, s.LocalAddr().(*net.UDPAddr))
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer func() { _ = conn.Close() }()

	if _, err := conn.Write(make([]byte, MaxDatagram*4)); err != nil {
		t.Fatalf("write oversized: %v", err)
	}
	probe, _ := json.Marshal(Probe{Type: "StunProbe", Nonce: "alive"})
	if _, err := conn.Write(probe); err != nil {
		t.Fatalf("write probe: %v", err)
	}
	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 2048)
	n, err := conn.Read(buf)
	if err != nil {
		t.Fatalf("the listener died on one oversized datagram: %v", err)
	}
	var reply Reply
	if err := json.Unmarshal(buf[:n], &reply); err != nil || reply.Nonce != "alive" {
		t.Fatalf("bad reply after the poison datagram: %q (%v)", reply.Nonce, err)
	}
}

// ---- STUN --------------------------------------------------------------------

func TestStunDropsOversizedProbesAndNonces(t *testing.T) {
	s, err := Start("127.0.0.1:0", config.NewLogger("error"))
	if err != nil {
		t.Fatalf("startStun: %v", err)
	}
	defer func() { _ = s.Close() }()

	dial := func() *net.UDPConn {
		conn, err := net.DialUDP("udp", nil, s.LocalAddr().(*net.UDPAddr))
		if err != nil {
			t.Fatalf("dial: %v", err)
		}
		return conn
	}
	silent := func(conn *net.UDPConn, what string) {
		t.Helper()
		_ = conn.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
		buf := make([]byte, 4096)
		if _, err := conn.Read(buf); err == nil {
			t.Fatalf("%s must get no reply", what)
		}
	}

	// A datagram past the size cap: dropped before it is even parsed, so the
	// reply can never be grown by a large echoed nonce.
	big := dial()
	defer func() { _ = big.Close() }()
	probe, _ := json.Marshal(Probe{Type: "StunProbe", Nonce: strings.Repeat("n", MaxDatagram)})
	if _, err := big.Write(probe); err != nil {
		t.Fatalf("write: %v", err)
	}
	silent(big, "an oversized datagram")

	// A nonce past its own cap, inside an otherwise legal datagram.
	longNonce := dial()
	defer func() { _ = longNonce.Close() }()
	probe, _ = json.Marshal(Probe{Type: "StunProbe", Nonce: strings.Repeat("n", MaxNonceBytes+1)})
	if len(probe) > MaxDatagram {
		t.Fatalf("test setup: the probe must be under the datagram cap, got %d", len(probe))
	}
	if _, err := longNonce.Write(probe); err != nil {
		t.Fatalf("write: %v", err)
	}
	silent(longNonce, "an over-long nonce")

	// And the real client's probe still works. The C++ StunClient's nonce is
	// "seat<N>-<32 hex lobby id>".
	ok := dial()
	defer func() { _ = ok.Close() }()
	probe, _ = json.Marshal(Probe{Type: "StunProbe", Nonce: "seat0-" + strings.Repeat("a", 32)})
	if _, err := ok.Write(probe); err != nil {
		t.Fatalf("write: %v", err)
	}
	_ = ok.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 2048)
	n, err := ok.Read(buf)
	if err != nil {
		t.Fatalf("a normal probe must still be answered: %v", err)
	}
	var reply Reply
	if err := json.Unmarshal(buf[:n], &reply); err != nil || reply.Nonce != "seat0-"+strings.Repeat("a", 32) {
		t.Fatalf("nonce not echoed verbatim: %q (%v)", reply.Nonce, err)
	}
}
