// Package handler owns the service's HTTP surface: the routes the outside world
// can reach, and nothing else. It knows how to mount a WebSocket handler and how
// to answer the two orchestrator probes; it knows nothing about lobbies, relays
// or STUN.
//
// The two probes answer DIFFERENT questions, which is the whole reason they are
// separate routes:
//
//   - /healthz — LIVENESS: "is this process broken in a way only a restart
//     fixes?" A failing liveness probe is an instruction to kill the machine, so
//     only conditions that a restart genuinely repairs belong in it (a dead UDP
//     read loop, a dead reaper). It must NOT report load, capacity or a
//     dependency being slow.
//   - /readyz — READINESS: "should a load balancer send new connections here?"
//     It goes down first during a drain, so the edge stops routing before the
//     listener closes.
//
// Getting that backwards is how a graceful deploy turns into a restart loop, so
// the two check sets are supplied separately and never merged here.
package handler

import (
	"context"
	"net/http"
	"sync/atomic"
	"time"

	"github.com/alexliesenfeld/health"
)

// kCheckTimeout bounds one probe evaluation. Every check this service registers
// is an atomic load or a mutex-guarded subtraction, so the timeout exists only so
// a future check that does real work cannot hang the endpoint past the
// orchestrator's own timeout (fly.toml uses 2 s).
const kCheckTimeout = time.Second

// Probe is one named health signal. Check returns nil when the component is
// healthy; the returned error is NOT surfaced to the caller (see New), it only
// decides up/down.
type Probe struct {
	Name  string
	Check func() error
}

// Config is the HTTP surface's composition. Live and Ready are deliberately two
// lists rather than one with flags: see the package comment.
type Config struct {
	// WS is the WebSocket upgrade handler, mounted at /ws.
	WS http.Handler
	// Live are the liveness components. Empty means "the process is running",
	// which is what the endpoint answered before it had any checks at all.
	Live []Probe
	// Ready are readiness components, evaluated in addition to the built-in
	// drain gate. Usually empty: on a single-instance deployment the only honest
	// readiness signal is "I have not been asked to shut down".
	Ready []Probe
}

// Handler is the service's http.Handler plus the drain switch behind /readyz.
type Handler struct {
	mux      *http.ServeMux
	live     health.Checker
	ready    health.Checker
	draining atomic.Bool
}

// New builds the mux. The route set is CLOSED: /ws, /healthz, /readyz, and a
// ServeMux 404 for everything else including path traversal (SECURITY.md
// "HTTP surface"). Nothing here discloses a version, a build or a count —
// health.WithDisabledDetails keeps both probe bodies at `{"status":"up"}`.
func New(cfg Config) *Handler {
	h := &Handler{mux: http.NewServeMux()}
	h.live = newChecker(cfg.Live)
	h.ready = newChecker(cfg.Ready)

	if cfg.WS != nil {
		h.mux.Handle("/ws", cfg.WS)
	}
	h.mux.Handle("/healthz", health.NewHandler(h.live))
	// The drain gate sits IN FRONT of the checker rather than inside it as a
	// check, for two reasons. It must be authoritative — a readiness endpoint
	// that forgets to drain is the bug this package exists to prevent, so it is
	// not something a caller can leave out of Config.Ready. And it must be
	// exact: the library dedupes check runs by comparing wall-clock timestamps,
	// and on a coarse-grained clock (Windows' is ~1 ms) two back-to-back
	// requests can share one result even with the cache "disabled". Draining is
	// a switch, not a measurement; it may not read stale for even one request.
	h.mux.Handle("/readyz", h.drainGate(health.NewHandler(h.ready)))
	return h
}

// drainGate answers 503 the instant Drain has been called, in the same response
// shape the health handler would have produced.
func (h *Handler) drainGate(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !h.draining.Load() {
			next.ServeHTTP(w, r)
			return
		}
		// Same no-store headers the library sets, so a caching proxy cannot
		// hold a "ready" answer across the drain.
		w.Header().Set("Cache-Control", "no-cache")
		w.Header().Set("Pragma", "no-cache")
		w.Header().Set("Expires", "Thu, 01 Jan 1970 00:00:00 GMT")
		_ = health.NewJSONResultWriter().Write(
			&health.CheckerResult{Status: health.StatusDown}, http.StatusServiceUnavailable, w, r)
	})
}

func (h *Handler) ServeHTTP(w http.ResponseWriter, r *http.Request) { h.mux.ServeHTTP(w, r) }

// Drain takes /readyz down. It is one-way: a drain is only ever the first step
// of a shutdown, and a service that could un-drain would invite a caller to use
// readiness as a load valve, which is exactly the misuse the package comment
// warns about.
func (h *Handler) Drain() { h.draining.Store(true) }

// Close stops the checkers' background workers. Safe to call once, at shutdown.
func (h *Handler) Close() {
	h.live.Stop()
	h.ready.Stop()
}

// newChecker wires Probes into a health.Checker.
//
// Caching is disabled: the library holds a result for a second by default, and
// a liveness answer a second stale is a second of a dead read loop still
// reporting "ok". Every probe this service registers is an atomic load or one
// mutex-guarded subtraction, so re-running them per poll costs less than the
// cache bookkeeping. (Disabling the cache is best-effort — dedup is by
// wall-clock timestamp, so on a coarse clock two requests in the same tick still
// share a result. That is why the drain gate does not rely on it; see New.)
func newChecker(probes []Probe) health.Checker {
	opts := []health.CheckerOption{
		health.WithDisabledDetails(),
		health.WithDisabledCache(),
		health.WithTimeout(kCheckTimeout),
	}
	for _, p := range probes {
		check := p.Check
		opts = append(opts, health.WithCheck(health.Check{
			Name:  p.Name,
			Check: func(context.Context) error { return check() },
		}))
	}
	return health.NewChecker(opts...)
}
