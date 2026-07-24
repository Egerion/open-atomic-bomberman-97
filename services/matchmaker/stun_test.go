package main

import (
	"encoding/json"
	"net"
	"testing"
	"time"
)

func TestStunEcho(t *testing.T) {
	s, err := startStun("127.0.0.1:0", newLogger("error"))
	if err != nil {
		t.Fatalf("startStun: %v", err)
	}
	defer func() { _ = s.Close() }()

	conn, err := net.DialUDP("udp", nil, s.LocalAddr().(*net.UDPAddr))
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer func() { _ = conn.Close() }()

	probe, _ := json.Marshal(stunProbe{Type: "StunProbe", Nonce: "abc-123"})
	if _, err := conn.Write(probe); err != nil {
		t.Fatalf("write probe: %v", err)
	}

	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 2048)
	n, err := conn.Read(buf)
	if err != nil {
		t.Fatalf("read reply: %v", err)
	}
	var reply stunReply
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
	s, err := startStun("127.0.0.1:0", newLogger("error"))
	if err != nil {
		t.Fatalf("startStun: %v", err)
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
