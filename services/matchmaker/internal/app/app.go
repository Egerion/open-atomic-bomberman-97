// Package app is the composition root and the process lifecycle: it builds the
// three listeners plus the HTTP surface from a Config, runs them, and drains
// them in the right order when its context is cancelled.
//
// It is a package rather than the body of main() for one reason: SHUTDOWN ORDER
// IS LOGIC, and logic in main() cannot be tested. app_test.go runs the whole
// service on ephemeral ports, watches /readyz go down before the listener
// closes, and asserts that Run returns inside its own budget.
//
// This is also the answer to "why no dependency-injection framework". The graph
// below is the entire object graph — nine constructor calls, one of them shared
// (relayTable), no cycles, no interfaces to select between. A generated injector
// would restate it in a second file and add a code-generation step to the build
// for a service whose wiring fits on one screen.
package app

import (
	"context"
	"errors"
	"fmt"
	"net"
	"net/http"
	"time"

	"go.uber.org/zap"

	"github.com/egedemirbas/open-bomberman/matchmaker/internal/config"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/handler"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/lobby"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/relay"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/stun"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/wsapi"
)

const (
	// kReadHeaderTimeout and kIdleTimeout bound a slow or bloated upgrade request
	// and reclaim keep-alive sockets that never make another request.
	// Deliberately NO ReadTimeout/WriteTimeout: those are whole-request
	// deadlines, and a WebSocket request lasts as long as the lobby does.
	kReadHeaderTimeout = 10 * time.Second
	kIdleTimeout       = 60 * time.Second

	// kStatsInterval is the cadence of every aggregated counter line in the
	// process, matching the relay's and the STUN echo's own maintain loops.
	kStatsInterval = 10 * time.Second

	// kReaperMissTolerance is how many reaper periods may pass with no completed
	// pass before LIVENESS fails. A pass is a few map walks under a mutex, so
	// missing six in a row is not slowness — it is a goroutine that is gone, and
	// then nothing evicts members or lobbies again for the life of the process.
	// Generous on purpose: a false liveness failure kills live lobbies.
	kReaperMissTolerance = 6
	kMinReaperDeadline   = time.Minute
)

// App is a bound, not-yet-serving instance. Binding in New rather than in Run is
// what lets a caller (and the test) learn the ephemeral ports, and what makes a
// port clash a startup error instead of a goroutine that logs and vanishes.
type App struct {
	cfg config.Config
	log *zap.Logger

	ln      net.Listener
	srv     *http.Server
	h       *handler.Handler
	ws      *wsapi.Server
	mgr     *lobby.Manager
	stunSrv *stun.Server
	relSrv  *relay.Server
}

// New binds all three listeners and wires the object graph. On failure it closes
// whatever it had already bound, so a caller never has to unwind a partial App.
func New(cfg config.Config, log *zap.Logger) (*App, error) {
	cfg = cfg.WithDefaults()
	a := &App{cfg: cfg, log: log}

	// One allocation table, two owners: the control plane mints and frees
	// entries, the UDP listener reads them. Constructed here so neither has to
	// reach into the other.
	relayTable := relay.NewTable(cfg.RelayIdle, relay.Limits{
		MaxAllocations:    cfg.MaxRelayAllocs,
		EgressBudgetBytes: cfg.RelayEgressBudgetBytes(),
	}, log)
	a.mgr = lobby.NewManager(cfg, relayTable, log)

	var err error
	if a.stunSrv, err = stun.Start(cfg.StunAddr, log); err != nil {
		return nil, fmt.Errorf("bind STUN listener %q: %w", cfg.StunAddr, err)
	}
	if a.relSrv, err = relay.Start(cfg.RelayAddr, relayTable, log); err != nil {
		a.closeListeners()
		return nil, fmt.Errorf("bind relay listener %q: %w", cfg.RelayAddr, err)
	}
	if a.ln, err = net.Listen("tcp", cfg.WSAddr); err != nil {
		a.closeListeners()
		return nil, fmt.Errorf("bind control plane %q: %w", cfg.WSAddr, err)
	}

	a.ws = wsapi.NewServer(a.mgr, cfg, log)
	a.h = handler.New(handler.Config{
		WS:   a.ws,
		Live: a.livenessProbes(),
		// Readiness carries only the built-in drain gate. Capacity deliberately
		// does NOT belong here: on a single-instance deployment reporting "not
		// ready" at the connection cap would take the only server out of
		// rotation exactly when it is busiest, and the orchestrator's answer to
		// a persistently unready machine is to restart it.
	})
	a.srv = &http.Server{
		Handler:           a.h,
		ReadHeaderTimeout: kReadHeaderTimeout,
		IdleTimeout:       kIdleTimeout,
		MaxHeaderBytes:    wsapi.MaxHeaderBytes,
	}
	return a, nil
}

// livenessProbes are the conditions a RESTART repairs and nothing else does.
// Each one is a failure the service could previously suffer while still
// answering "ok" — SECURITY.md F6 is exactly that story for the two read loops.
func (a *App) livenessProbes() []handler.Probe {
	deadline := a.cfg.HeartbeatInterval * kReaperMissTolerance
	if deadline < kMinReaperDeadline {
		deadline = kMinReaperDeadline
	}
	return []handler.Probe{
		{Name: "stun_listener", Check: func() error {
			if !a.stunSrv.Alive() {
				return errors.New("STUN read loop has stopped")
			}
			return nil
		}},
		{Name: "relay_listener", Check: func() error {
			if !a.relSrv.Alive() {
				return errors.New("relay read loop has stopped")
			}
			return nil
		}},
		{Name: "lobby_reaper", Check: func() error {
			if age := a.mgr.ReaperAge(); age > deadline {
				return fmt.Errorf("no reaper pass for %s", age.Round(time.Second))
			}
			return nil
		}},
	}
}

// Addrs reports where the process actually bound, which is not what the config
// says whenever a port is 0.
func (a *App) Addrs() (ws, stunAddr, relayAddr string) {
	return a.ln.Addr().String(), a.stunSrv.LocalAddr().String(), a.relSrv.LocalAddr().String()
}

// Run serves until ctx is cancelled or the HTTP server fails, then drains. It
// always closes everything it owns before returning.
func (a *App) Run(ctx context.Context) error {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()

	go a.mgr.RunReaper(ctx)
	go a.ws.RunStats(ctx, kStatsInterval)

	a.logStartup()

	serveErr := make(chan error, 1)
	go func() {
		var err error
		if a.cfg.TLSCert != "" && a.cfg.TLSKey != "" {
			err = a.srv.ServeTLS(a.ln, a.cfg.TLSCert, a.cfg.TLSKey)
		} else {
			err = a.srv.Serve(a.ln)
		}
		if errors.Is(err, http.ErrServerClosed) {
			err = nil
		}
		serveErr <- err
	}()

	var runErr error
	select {
	case <-ctx.Done():
	case err := <-serveErr:
		runErr = err
		if err != nil {
			a.log.Error("HTTP server stopped", zap.Error(err))
		}
	}
	if err := a.drain(); err != nil && runErr == nil {
		runErr = err
	}
	return runErr
}

// drain is the shutdown order, and the order is the point:
//
//  1. /readyz down, so the edge stops routing NEW connections here;
//  2. wait DrainDelay, giving it time to notice (0 by default — with one
//     instance the pause only lengthens the outage);
//  3. close the HTTP listener and let in-flight plain requests finish;
//  4. close the WebSocket sessions with a real close frame, because they are
//     hijacked connections and step 3 neither closes nor waits for them;
//  5. stop the UDP listeners, each of which now waits for its own read loop.
//
// The whole sequence is bounded by ShutdownTimeout: it has to fit inside the
// platform's SIGTERM→SIGKILL window, and what it covers is closing sockets, not
// finishing work.
func (a *App) drain() error {
	deadline := time.Now().Add(a.cfg.ShutdownTimeout)
	a.log.Info("draining",
		zap.Duration("drain_delay", a.cfg.DrainDelay),
		zap.Duration("shutdown_timeout", a.cfg.ShutdownTimeout))

	a.h.Drain()
	if d := a.cfg.DrainDelay; d > 0 {
		if until := time.Until(deadline); d > until {
			d = until
		}
		if d > 0 {
			time.Sleep(d)
		}
	}

	ctx, cancel := context.WithDeadline(context.Background(), deadline)
	defer cancel()

	err := a.srv.Shutdown(ctx)
	if wsErr := a.ws.Shutdown(ctx); wsErr != nil && err == nil {
		err = wsErr
	}
	a.h.Close()
	a.closeListeners()
	if err != nil {
		a.log.Warn("drain did not finish inside the budget", zap.Error(err))
	} else {
		a.log.Info("drained")
	}
	return err
}

// Close releases everything without a drain. Only for a caller that never ran.
func (a *App) Close() {
	a.closeListeners()
	if a.ln != nil {
		_ = a.ln.Close()
	}
}

func (a *App) closeListeners() {
	if a.stunSrv != nil {
		_ = a.stunSrv.Close()
	}
	if a.relSrv != nil {
		_ = a.relSrv.Close()
	}
}

func (a *App) logStartup() {
	wsAddr, stunAddr, relayAddr := a.Addrs()
	scheme := "ws (terminate TLS at the edge)"
	if a.cfg.TLSCert != "" && a.cfg.TLSKey != "" {
		scheme = "wss (standalone TLS)"
	}
	a.log.Info("WebSocket control plane listening",
		zap.String("addr", wsAddr), zap.String("scheme", scheme))
	a.log.Info("STUN echo listening", zap.String("addr", stunAddr))
	a.log.Info("UDP relay listening", zap.String("addr", relayAddr),
		zap.String("advertise", a.cfg.AdvertisedRelay()), zap.Duration("idle", a.cfg.RelayIdle))
	// The relay is the only component that spends money, so its two cost caps
	// are stated at startup next to the address they apply to. The budget is
	// per-process and per-uptime: printing it here is also the reminder that it
	// started again at zero when this line was written.
	a.log.Info("relay cost caps (refuse NEW allocations only; a match already forwarding is never cut)",
		zap.Int("max_relay_allocs", a.cfg.MaxRelayAllocs),
		zap.Int("relay_budget_gb", a.cfg.RelayBudgetGB),
		zap.Int64("relay_budget_bytes", a.cfg.RelayEgressBudgetBytes()),
		zap.String("budget_scope", "this process only — resets on restart/deploy"))
	if !config.HasRoutableHost(a.cfg.AdvertisedRelay()) {
		// A wildcard listen address is not something a client can dial; without
		// -relay-advertise every RelayAllocated would hand out a dead address.
		a.log.Warn("advertised relay_addr has no routable host — set -relay-advertise to the public host:port",
			zap.String("advertise", a.cfg.AdvertisedRelay()))
	}
	a.log.Info("control-plane limits",
		zap.Int("max_conns", a.cfg.MaxConns), zap.Int("max_conns_per_ip", a.cfg.MaxConnsPerIP),
		zap.Int("max_lobbies", a.cfg.MaxLobbies), zap.Duration("conn_idle_timeout", a.cfg.ConnIdleTimeout),
		zap.String("client_ip_header", a.cfg.ClientIPHeader))
	if a.cfg.ClientIPHeader == "" {
		// Behind an edge proxy every connection arrives from a private address,
		// so ratelimit.SourceKey declines to cap it and -max-conns-per-ip does
		// nothing. Say so at startup rather than letting an operator assume a
		// live cap.
		a.log.Warn("no -client-ip-header set: per-IP limits apply only to directly connected clients")
	}
}
