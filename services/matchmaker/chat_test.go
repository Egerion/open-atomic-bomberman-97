package main

import (
	"strings"
	"testing"
	"time"
)

// Chat (PROTOCOL.md §7) — the PORT-ONLY lobby chat relay. The rules worth
// pinning are the safety ones: it never leaves the sender's lobby, it never
// carries a seat/name the sender chose, it is capped in length and in rate, and
// it is rejected rather than repaired.

func sendChat(m *Manager, c clientConn, text string) {
	dispatchMap(m, c, map[string]any{"type": TypeChat, "text": text})
}

func TestChatFansOutToTheWholeLobbyIncludingTheSender(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	host.reset()
	guest.reset()

	sendChat(m, guest, "hello there")

	for name, c := range map[string]*fakeConn{"host": host, "guest": guest} {
		got := lastTyped[chatRelayMsg](t, c, TypeChat)
		if got.Text != "hello there" {
			t.Fatalf("%s: text should pass through verbatim, got %q", name, got.Text)
		}
		if got.Seat != 1 || got.Name != "Ada" {
			t.Fatalf("%s: seat/name must come from the roster, got seat=%d name=%q",
				name, got.Seat, got.Name)
		}
	}
}

func TestChatNeverLeavesTheSendersLobby(t *testing.T) {
	m := newTestManager(t)
	hostA := newFakeConn("hostA")
	createLobby(t, m, hostA, nil)
	hostB := newFakeConn("hostB")
	createLobby(t, m, hostB, nil)
	hostA.reset()
	hostB.reset()

	sendChat(m, hostA, "only for lobby A")

	if !hostA.has(TypeChat) {
		t.Fatal("the sender's own lobby should receive the message")
	}
	if hostB.has(TypeChat) {
		t.Fatal("a different lobby must never see it")
	}
}

func TestChatFromASeatlessConnectionIsRefused(t *testing.T) {
	m := newTestManager(t)
	stray := newFakeConn("stray")

	sendChat(m, stray, "anybody there")

	if stray.has(TypeChat) {
		t.Fatal("a connection with no seat must not get a relay back")
	}
	e := lastTyped[errorMsg](t, stray, TypeError)
	if e.Code != "not_in_lobby" {
		t.Fatalf("expected not_in_lobby, got %q", e.Code)
	}
}

func TestChatSeatAndNameCannotBeSpoofed(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	host.reset()

	// The guest claims the host's seat and name in its own frame.
	dispatchMap(m, guest, map[string]any{
		"type": TypeChat, "seat": 0, "name": "Ege", "text": "trust me",
	})

	got := lastTyped[chatRelayMsg](t, host, TypeChat)
	if got.Seat != 1 || got.Name != "Ada" {
		t.Fatalf("the server's roster must win, got seat=%d name=%q", got.Seat, got.Name)
	}
}

func TestChatRejectsOverLongAndUnprintableText(t *testing.T) {
	m := newTestManager(t)
	host := newFakeConn("host")
	createLobby(t, m, host, nil)

	cases := []struct {
		name string
		text string
		code string
	}{
		{"too long", strings.Repeat("x", kChatMaxBytes+1), "chat_too_long"},
		{"control character", "hello\x07world", "chat_invalid"},
		{"newline", "line one\nline two", "chat_invalid"},
		// encoding/json turns the sender's invalid bytes into U+FFFD on the way
		// in; a body that arrives already repaired is refused rather than
		// forwarded as if it were what the sender typed.
		{"invalid utf-8", "bad \xff\xfe", "chat_invalid"},
		{"blank", "   ", "chat_invalid"},
		{"empty", "", "chat_invalid"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			host.reset()
			sendChat(m, host, tc.text)
			if host.has(TypeChat) {
				t.Fatal("a rejected message must not be relayed at all")
			}
			e := lastTyped[errorMsg](t, host, TypeError)
			if e.Code != tc.code {
				t.Fatalf("expected %q, got %q", tc.code, e.Code)
			}
		})
	}

	// The boundary itself is legal, and non-ASCII is the client's problem to
	// render, not the relay's to censor.
	host.reset()
	exact := strings.Repeat("y", kChatMaxBytes)
	sendChat(m, host, exact)
	if got := lastTyped[chatRelayMsg](t, host, TypeChat); got.Text != exact {
		t.Fatalf("a message of exactly kChatMaxBytes must pass unchanged")
	}
	host.reset()
	sendChat(m, host, "merhaba dünya")
	if got := lastTyped[chatRelayMsg](t, host, TypeChat); got.Text != "merhaba dünya" {
		t.Fatalf("non-ASCII must pass through verbatim, got %q", got.Text)
	}
}

func TestChatRateLimitDropsTheFloodAndRecoversWithTime(t *testing.T) {
	m := newTestManager(t)
	now := time.Now()
	m.now = func() time.Time { return now }

	host := newFakeConn("host")
	createLobby(t, m, host, nil)
	host.reset()

	// The bucket starts full: kChatBurstMsgs go out back to back, the next is
	// dropped outright (no Error, no truncated relay — §7).
	for i := 0; i < kChatBurstMsgs; i++ {
		sendChat(m, host, "burst")
	}
	if got := host.countOfType(TypeChat); got != kChatBurstMsgs {
		t.Fatalf("the burst allowance should pass: want %d relays, got %d", kChatBurstMsgs, got)
	}
	host.reset()
	sendChat(m, host, "one too many")
	if host.has(TypeChat) {
		t.Fatal("the over-rate message must be dropped")
	}
	if host.has(TypeError) {
		t.Fatal("a dropped message must not be answered (that would amplify a flood)")
	}

	// One credit's worth of silence buys exactly one more line.
	now = now.Add(kChatCreditPerMsgMs * time.Millisecond)
	sendChat(m, host, "after waiting")
	if got := lastTyped[chatRelayMsg](t, host, TypeChat); got.Text != "after waiting" {
		t.Fatalf("the bucket should refill over time, got %q", got.Text)
	}
}

func TestChatKeepsWorkingOnceTheMatchHasStarted(t *testing.T) {
	// The online setup screens (roster + level) run AFTER StartMatch locks the
	// lobby, and the overlay stays live there — so a LOCKED lobby must still
	// relay. Only an evicted one goes quiet, and that one no longer exists.
	m := newTestManager(t)
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")
	setReady(m, host, true)
	setReady(m, guest, true)
	dispatchMap(m, host, map[string]any{
		"type": TypeStartMatch, "lobby_id": lc.LobbyID, "host_token": lc.HostToken,
	})
	guest.reset()

	sendChat(m, host, "gl hf")
	if got := lastTyped[chatRelayMsg](t, guest, TypeChat); got.Text != "gl hf" {
		t.Fatalf("a LOCKED lobby must still carry chat, got %q", got.Text)
	}
}
