package lobby

// This file is the server-side lobby state machine of design §5.2, on its own
// so the transitions are one readable thing instead of six `lb.state != …`
// comparisons scattered through the handlers.
//
// It deliberately does NOT live in a `model` package. The state is one field of
// `lobby`, which is guarded by `Manager.mu`; moving the entity across a package
// boundary would mean exporting the fields that mutex protects, and the shared
// value types a domain model would otherwise hold (Candidate, RosterEntry,
// Topology, PublicLobby) already live in `protocol` — which for this service is
// not a DTO layer but the frozen contract itself. See README "Layout".

// lobbyState is the server-side per-lobby machine (design §5.2).
//
//	OPEN ──StartMatch──► LOCKED ──all connected | grace──► IN_PROGRESS
//	  ▲                                                         │
//	  └──────────────────── MatchOver ◄────────────────────────-┘
//
//	any ──last member leaves / all time out──► EVICTED (terminal, then freed)
type lobbyState int

const (
	stateOpen       lobbyState = iota // accepting joins; roster + candidates live
	stateLocked                       // StartMatch broadcast; no new joins; candidate relay still live for the punch
	stateInProgress                   // match running; control plane dormant; joins → in_progress
	stateEvicted                      // freed (empty / all timed out)
)

func (s lobbyState) String() string {
	switch s {
	case stateOpen:
		return "OPEN"
	case stateLocked:
		return "LOCKED"
	case stateInProgress:
		return "IN_PROGRESS"
	case stateEvicted:
		return "EVICTED"
	default:
		return "?"
	}
}

// acceptsJoins reports whether a JoinByCode may seat a new member. It is also
// what decides whether a public lobby is worth listing: a row a browser cannot
// join is noise.
func (s lobbyState) acceptsJoins() bool { return s == stateOpen }

// canStart reports whether StartMatch may be honoured. Same predicate as
// acceptsJoins today, named separately because they answer different questions
// and only one of them has to change if §5.2 grows a state.
func (s lobbyState) canStart() bool { return s == stateOpen }

// relaysCandidates reports whether the rendezvous window is still open. It stays
// open through LOCKED on purpose: StartMatch is broadcast before the peers have
// punched, so the candidate exchange has to outlive the lock.
func (s lobbyState) relaysCandidates() bool { return s == stateOpen || s == stateLocked }

// isLive reports whether the lobby still exists as far as its members are
// concerned. An evicted lobby accepts nothing — it is on its way out of the map.
func (s lobbyState) isLive() bool { return s != stateEvicted }
