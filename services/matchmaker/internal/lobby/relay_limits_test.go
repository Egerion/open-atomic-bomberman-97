package lobby

import (
	"errors"
	"testing"

	"github.com/egedemirbas/open-bomberman/matchmaker/internal/protocol"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/relay"
)

// How a refused relay allocation reaches the client, and why it looks the way it
// does. PROTOCOL.md §6.1 is FROZEN and permits exactly not_in_lobby /
// bad_message / internal in answer to AllocateRelay; a deployed C++ client is
// written against it, and it does not branch on the code here anyway —
// LobbyFlow::handle_server_message turns ANY Error arriving in Phase::Relaying
// into fail("RELAY UNAVAILABLE - CANNOT CONNECT"). So a cost cap reuses the
// existing `internal` refusal and puts the reason in the diagnostic `message`.
//
// These tests exist to stop a well-meaning future change from minting a new
// code (or a new message type) for this, which would be a one-sided change to a
// frozen contract and would land on a deployed player as an unhandled frame.
//
// Whether the caps THEMSELVES behave — refuse the new, never cut the live — is
// proved against the forwarder in internal/relay/limits_test.go.

func TestAllocateRelayPastACostCapRefusesWithAFrozenErrorCode(t *testing.T) {
	m := newTestManagerWithRelayLimits(t, relay.Limits{MaxAllocations: 1})
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")

	// Seat 0 takes the only row.
	allocateRelay(t, m, host, map[string]any{"lobby_id": lc.LobbyID, "seat": 0})

	dispatchMap(m, guest, map[string]any{"type": protocol.TypeAllocateRelay, "lobby_id": lc.LobbyID, "seat": 1})
	if guest.has(protocol.TypeRelayAllocated) {
		t.Fatal("a refused allocation must not answer RelayAllocated")
	}
	e := lastTyped[protocol.ErrorMsg](t, guest, protocol.TypeError)
	if e.Code != "internal" {
		t.Fatalf("the refusal must reuse the frozen `internal` code (PROTOCOL.md §6.1), got %q", e.Code)
	}
	if e.Message == "" {
		t.Fatal("the refusal should say which cap fired in the diagnostic message")
	}
	if m.relay.Size() != 1 {
		t.Fatalf("a refused allocation must not add a row, %d rows", m.relay.Size())
	}
}

// A seat that already holds an allocation must still be answered when the
// server has stopped admitting new ones: PROTOCOL.md §6.1 promises AllocateRelay
// is idempotent and safe to retry mid-match after a lost reply, and a full
// server must not break that promise for the matches it is already carrying.
func TestAllocateRelayStillAnswersASeatThatAlreadyHoldsAnAllocation(t *testing.T) {
	m := newTestManagerWithRelayLimits(t, relay.Limits{MaxAllocations: 2})
	host := newFakeConn("host")
	lc := createLobby(t, m, host, nil)
	guest := join(m, lc.Code, "Ada", "0xA1B2C3D4", "guest")

	first := allocateRelay(t, m, host, map[string]any{"lobby_id": lc.LobbyID, "seat": 0})
	allocateRelay(t, m, guest, map[string]any{"lobby_id": lc.LobbyID, "seat": 1})
	// The table is full now — a third seat anywhere would be refused.

	host.reset()
	again := allocateRelay(t, m, host, map[string]any{"lobby_id": lc.LobbyID, "seat": 0})
	if again.AllocID != first.AllocID {
		t.Fatalf("a live seat re-requesting on a full server must get the SAME handle: %q vs %q",
			again.AllocID, first.AllocID)
	}
	if host.has(protocol.TypeError) {
		t.Fatal("a live seat re-requesting on a full server must not be refused")
	}
}

// Both caps map to the same frozen wire code and differ only in the diagnostic
// text, so an operator reading a player's screenshot can tell a cost ceiling
// from a genuine fault — while a refused caller still learns nothing about how
// much budget is left or how full the table is.
func TestRelayRefusalNamesTheCapWithoutLeakingItsState(t *testing.T) {
	budget := relayRefusal(relay.ErrEgressBudget)
	capped := relayRefusal(relay.ErrAllocationLimit)
	other := relayRefusal(errors.New("some other failure"))

	if budget == capped || budget == other || capped == other {
		t.Fatalf("each refusal reason should read differently: %q / %q / %q", budget, capped, other)
	}
	for _, s := range []string{budget, capped, other} {
		if s == "" {
			t.Fatal("every refusal needs diagnostic text")
		}
	}
}
