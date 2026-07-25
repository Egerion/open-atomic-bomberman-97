// Command matchmaker is Open Bomberman's online-multiplayer signaling / lobby
// control plane, STUN echo, and UDP relay forwarder (ADR-0011, design §§1–2,
// §4, §5.2). It does NOT simulate and never sees game State — it brokers lobby
// state, echoes reflexive addresses, and forwards OPAQUE relayed datagrams for
// peers whose hole-punch failed.
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

	relay, err := startRelay(cfg.RelayAddr, mgr.relay, log)
	if err != nil {
		log.Error("relay listener failed to bind", "addr", cfg.RelayAddr, "err", err)
		os.Exit(1)
	}
	defer func() { _ = relay.Close() }()
	log.Info("UDP relay listening", "addr", relay.LocalAddr().String(),
		"advertise", cfg.relayAdvertise(), "idle", cfg.RelayIdle)
	if !hasRoutableHost(cfg.relayAdvertise()) {
		// A wildcard listen address is not something a client can dial; without
		// -relay-advertise every RelayAllocated would hand out a dead address.
		log.Warn("advertised relay_addr has no routable host — set -relay-advertise to the public host:port",
			"advertise", cfg.relayAdvertise())
	}

	ws := newWSServer(mgr, cfg, log)
	srv := &http.Server{
		Addr:    cfg.WSAddr,
		Handler: ws.handler(),
		// ReadHeaderTimeout + MaxHeaderBytes bound a slow or bloated upgrade
		// request; IdleTimeout reclaims keep-alive sockets that never make
		// another request. Deliberately NO ReadTimeout/WriteTimeout: those are
		// whole-request deadlines, and a WebSocket request lasts as long as the
		// lobby does.
		ReadHeaderTimeout: 10 * time.Second,
		IdleTimeout:       60 * time.Second,
		MaxHeaderBytes:    wsMaxHeaderBytes,
	}
	log.Info("control-plane limits",
		"max_conns", cfg.MaxConns, "max_conns_per_ip", cfg.MaxConnsPerIP,
		"max_lobbies", cfg.MaxLobbies, "conn_idle_timeout", cfg.ConnIdleTimeout,
		"client_ip_header", cfg.ClientIPHeader)
	if cfg.ClientIPHeader == "" {
		// Behind an edge proxy every connection arrives from a private address,
		// so perIPKey declines to cap it and -max-conns-per-ip does nothing. Say
		// so at startup rather than letting an operator assume a live cap.
		log.Warn("no -client-ip-header set: per-IP limits apply only to directly connected clients")
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
