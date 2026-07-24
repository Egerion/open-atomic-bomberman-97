// Command matchmaker is Open Bomberman's online-multiplayer signaling / lobby
// control plane + STUN echo (ADR-0011, design §§1–2, §5.2). Phase 1a: it does
// NOT simulate and never sees game State — it only brokers lobby state and
// reflexive addresses. The UDP relay forwarder (Phase 2) is stubbed (relay.go).
package main

import (
	"context"
	"errors"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
)

func main() {
	cfg := parseConfig(os.Args[1:])
	log := newLogger(cfg.LogLevel)

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	mgr := NewManager(cfg, log)
	go mgr.runReaper(ctx)

	stun, err := startStun(cfg.StunAddr, log)
	if err != nil {
		log.Error("STUN listener failed to bind", "addr", cfg.StunAddr, "err", err)
		os.Exit(1)
	}
	defer func() { _ = stun.Close() }()
	log.Info("STUN echo listening", "addr", stun.LocalAddr().String())

	ws := &wsServer{mgr: mgr, log: log}
	srv := &http.Server{
		Addr:              cfg.WSAddr,
		Handler:           ws.handler(),
		ReadHeaderTimeout: 10 * time.Second,
	}

	go func() {
		var err error
		if cfg.TLSCert != "" && cfg.TLSKey != "" {
			log.Info("WebSocket control plane listening (wss, standalone TLS)", "addr", cfg.WSAddr)
			err = srv.ListenAndServeTLS(cfg.TLSCert, cfg.TLSKey)
		} else {
			log.Info("WebSocket control plane listening (ws; terminate TLS at the edge)", "addr", cfg.WSAddr)
			err = srv.ListenAndServe()
		}
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Error("HTTP server stopped", "err", err)
			stop()
		}
	}()

	<-ctx.Done()
	log.Info("shutting down")
	shutCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_ = srv.Shutdown(shutCtx)
}
