package wsapi

import (
	"context"
	"encoding/json"
	"net/http"
	"sync"
	"sync/atomic"
	"time"

	"github.com/coder/websocket"
	"go.uber.org/zap"

	"github.com/egedemirbas/open-bomberman/matchmaker/internal/config"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/lobby"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/ratelimit"
)

const (
	// ReadLimit caps ONE inbound control frame. The largest legitimate client
	// frame is a Candidates list — at most kMaxCandidates entries of an address
	// each, so a few KiB — and everything else is tens of bytes. 8 KiB leaves
	// several times that headroom while keeping the parse cost of a hostile
	// frame small (Go's encoding/json also refuses beyond 10000 levels of
	// nesting, which 8 KiB cannot reach). Contract: PROTOCOL.md §8.
	ReadLimit   = 8 << 10
	kWriteWait  = 10 * time.Second
	kSendBuffer = 64

	// MaxHeaderBytes bounds the HTTP request headers of an upgrade.
	MaxHeaderBytes = 16 << 10

	// kCloseWait bounds the graceful close handshake, which happens on its own
	// goroutine so it can never block the Manager (see conn.disconnect).
	kCloseWait = 5 * time.Second

	// kShutdownReason is what a client is told when the process drains. The C++
	// LobbyFlow reconnects lazily on the next action, so a clean close during a
	// deploy costs a player nothing.
	kShutdownReason = "server shutting down"
)

// Server bridges HTTP/WebSocket to the transport-agnostic Manager, and owns
// the admission control in front of it: the Manager's per-connection state is
// only bounded because this refuses to accept an unbounded number of sockets.
//
// It is the /ws handler ONLY. The route table, the health endpoints and the
// drain switch live in the handler package — this type has no opinion about
// what else the process serves.
type Server struct {
	mgr *lobby.Manager
	cfg config.Config
	log *zap.Logger

	mu    sync.Mutex
	live  int            // total open connections
	perIP map[string]int // client key -> open connections; bounded by `live`
	// conns is every open socket, so a drain can close them deliberately instead
	// of letting the process exit underneath them. http.Server.Shutdown cannot do
	// it: a WebSocket is a HIJACKED connection and Shutdown neither closes nor
	// waits for those. Keyed on a pointer this package minted, and bounded by
	// `live`, so it satisfies the same rules as perIP (SECURITY.md rules 2 and 3).
	conns map[*conn]struct{}

	draining  bool
	drained   chan struct{} // closed once draining && live == 0
	drainOnce sync.Once

	refused atomic.Uint64
}

func NewServer(mgr *lobby.Manager, cfg config.Config, log *zap.Logger) *Server {
	return &Server{
		mgr:     mgr,
		cfg:     cfg.WithDefaults(),
		log:     log,
		perIP:   map[string]int{},
		conns:   map[*conn]struct{}{},
		drained: make(chan struct{}),
	}
}

// ServeHTTP handles one WebSocket upgrade. Mount it at /ws.
func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) { s.serveWS(w, r) }

// Shutdown closes every open connection with a close frame and waits for them to
// go away, bounded by ctx.
//
// Call it AFTER http.Server.Shutdown has closed the listener: this refuses
// nothing itself, so a socket accepted in between would be missed and the wait
// would then run to the ctx deadline instead of finishing early.
func (s *Server) Shutdown(ctx context.Context) error {
	s.mu.Lock()
	s.draining = true
	open := make([]*conn, 0, len(s.conns))
	for c := range s.conns {
		open = append(open, c)
	}
	empty := s.live == 0
	s.mu.Unlock()

	if empty {
		s.signalDrained()
	}
	// Outside the lock on purpose: Disconnect cancels inline and finishes the
	// close handshake on its own goroutine (SECURITY.md F10), and holding s.mu
	// across a fan-out is the shape that bug had.
	for _, c := range open {
		c.Disconnect(kShutdownReason)
	}

	select {
	case <-s.drained:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

func (s *Server) signalDrained() { s.drainOnce.Do(func() { close(s.drained) }) }

// RunStats emits ONE aggregated admission line per interval, and only when a
// tally moved — the same discipline as `relay stats` and `control-plane drops`
// (SECURITY.md rule 4). Without it the refusal counter is invisible: a service
// sitting at its connection cap looks exactly like an idle one from the outside.
func (s *Server) RunStats(ctx context.Context, every time.Duration) {
	t := time.NewTicker(every)
	defer t.Stop()
	var last uint64
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			cur := s.refused.Load()
			if cur == last {
				continue
			}
			s.mu.Lock()
			live, keys := s.live, len(s.perIP)
			s.mu.Unlock()
			s.log.Info("ws admission",
				zap.Uint64("refused", cur), zap.Int("live", live), zap.Int("client_keys", keys))
			last = cur
		}
	}
}

// reserve takes one connection slot, or reports why it cannot.
//
// perIP is a map keyed by an attacker-chosen value, which is normally exactly
// the thing not to do — it is safe here ONLY because an entry can exist only
// while a connection is open, entries are deleted at zero, and the total is
// capped by MaxConns. It can therefore never hold more than MaxConns keys.
func (s *Server) reserve(key string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.cfg.MaxConns > 0 && s.live >= s.cfg.MaxConns {
		return false
	}
	if key != "" && s.cfg.MaxConnsPerIP > 0 && s.perIP[key] >= s.cfg.MaxConnsPerIP {
		return false
	}
	s.live++
	if key != "" {
		s.perIP[key]++
	}
	return true
}

func (s *Server) release(key string) {
	s.mu.Lock()
	s.live--
	if key != "" {
		if n := s.perIP[key] - 1; n > 0 {
			s.perIP[key] = n
		} else {
			delete(s.perIP, key)
		}
	}
	empty := s.draining && s.live == 0
	s.mu.Unlock()
	if empty {
		s.signalDrained() // the last socket left during a drain; unblock Shutdown
	}
}

func (s *Server) track(c *conn) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.conns[c] = struct{}{}
}

func (s *Server) untrack(c *conn) {
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.conns, c)
}

func (s *Server) serveWS(w http.ResponseWriter, r *http.Request) {
	// Admission FIRST, before the upgrade: refusing costs one small HTTP reply
	// instead of a socket, two goroutines and a send buffer.
	key := ratelimit.SourceKey(ratelimit.ClientIP(r, s.cfg.ClientIPHeader))
	if !s.reserve(key) {
		s.refused.Add(1) // counted, never logged per refusal
		http.Error(w, "server at capacity", http.StatusServiceUnavailable)
		return
	}
	defer s.release(key)

	c, err := websocket.Accept(w, r, &websocket.AcceptOptions{
		// Native clients (IXWebSocket) send no Origin; browsers may be cross-origin.
		// Authn is by lobby code + host_token, not Origin, and TLS terminates at
		// the edge — so Origin checking is intentionally disabled here.
		InsecureSkipVerify: true,
	})
	if err != nil {
		s.log.Debug("ws accept failed", zap.Error(err))
		return
	}
	c.SetReadLimit(ReadLimit)

	connID, err := lobby.NewHandle()
	if err != nil {
		_ = c.Close(websocket.StatusInternalError, "handle alloc failed")
		return
	}
	ctx, cancel := context.WithCancel(r.Context())
	wc := &conn{
		connID: connID,
		c:      c,
		addr:   r.RemoteAddr,
		out:    make(chan []byte, kSendBuffer),
		cancel: cancel,
		log:    s.log,
	}
	// Registered so the Manager can rate-limit and idle-reap this socket from
	// the moment it exists, seat or no seat; removed unconditionally on the way
	// out so neither map outlives the connection.
	s.mgr.AddConn(wc, key)
	s.track(wc)
	defer func() {
		s.untrack(wc)
		s.mgr.RemoveConn(wc)
		wc.Disconnect("connection closed")
	}()

	go wc.writeLoop(ctx)
	s.readLoop(ctx, wc)
}

func (s *Server) readLoop(ctx context.Context, wc *conn) {
	for {
		typ, data, err := wc.c.Read(ctx)
		if err != nil {
			return // client closed, read limit exceeded, or context cancelled
		}
		if typ != websocket.MessageText {
			continue // control plane is JSON text; ignore binary
		}
		s.mgr.Dispatch(wc, data)
	}
}

// conn adapts one WebSocket to clientConn. A single writer goroutine drains
// the out channel so the Manager can send while holding its lock without ever
// blocking or racing a concurrent writer.
type conn struct {
	connID    string
	c         *websocket.Conn
	addr      string
	out       chan []byte
	cancel    context.CancelFunc
	log       *zap.Logger
	closeOnce sync.Once
}

func (w *conn) ID() string     { return w.connID }
func (w *conn) Remote() string { return w.addr }

func (w *conn) Send(v any) {
	data, err := json.Marshal(v)
	if err != nil {
		w.log.Error("marshal outbound", zap.Error(err))
		return
	}
	select {
	case w.out <- data:
	default:
		// Slow/stuck consumer: drop it rather than block the Manager.
		w.Disconnect("send buffer overflow")
	}
}

// disconnect tears the socket down. It is called from inside the Manager's lock
// (a broadcast that overflows a stuck consumer's buffer, or the heartbeat
// reaper), so it MUST NOT do network I/O inline: the close handshake writes a
// frame, and a peer that has stopped reading would then stall every lobby on
// the server behind one mutex. Cancelling the context stops this connection
// immediately; the handshake finishes on its own goroutine, one per connection
// thanks to closeOnce.
func (w *conn) Disconnect(reason string) {
	w.closeOnce.Do(func() {
		w.cancel()
		go func() {
			done := make(chan struct{})
			go func() {
				defer close(done)
				_ = w.c.Close(websocket.StatusNormalClosure, truncateReason(reason))
			}()
			select {
			case <-done:
			case <-time.After(kCloseWait):
				_ = w.c.CloseNow() // peer will not complete the handshake
			}
		}()
	})
}

func (w *conn) writeLoop(ctx context.Context) {
	for {
		select {
		case <-ctx.Done():
			return
		case data := <-w.out:
			wctx, cancel := context.WithTimeout(ctx, kWriteWait)
			err := w.c.Write(wctx, websocket.MessageText, data)
			cancel()
			if err != nil {
				w.Disconnect("write error")
				return
			}
		}
	}
}

// truncateReason keeps a close reason within the WebSocket 123-byte limit.
func truncateReason(s string) string {
	if len(s) > 120 {
		return s[:120]
	}
	return s
}
