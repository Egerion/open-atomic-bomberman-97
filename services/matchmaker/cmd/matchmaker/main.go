// Command matchmaker is Open Bomberman's online-multiplayer signaling / lobby
// control plane, STUN echo, and UDP relay forwarder (ADR-0011, design §§1–2,
// §4, §5.2). It does NOT simulate and never sees game State — it brokers lobby
// state, echoes reflexive addresses, and forwards OPAQUE relayed datagrams for
// peers whose hole-punch failed.
//
// This file is WIRING ONLY: parse the configuration, construct the three
// listeners and the state they share, and shut them down. Every decision it
// makes lives in one of the internal packages.
package main

import (
	"context"
	"errors"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/egedemirbas/open-bomberman/matchmaker/internal/config"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/lobby"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/relay"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/stun"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/wsapi"
)

func main() {
	cfg := config.Parse(os.Args[1:])
	log := config.NewLogger(cfg.LogLevel)

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	// One allocation table, two owners: the control plane mints and frees
	// entries, the UDP listener reads them. Constructed here so neither has to
	// reach into the other.
	relayTable := relay.NewTable(cfg.RelayIdle, log)

	mgr := lobby.NewManager(cfg, relayTable, log)
	go mgr.RunReaper(ctx)

	stunSrv, err := stun.Start(cfg.StunAddr, log)
	if err != nil {
		log.Error("STUN listener failed to bind", "addr", cfg.StunAddr, "err", err)
		os.Exit(1)
	}
	defer func() { _ = stunSrv.Close() }()
	log.Info("STUN echo listening", "addr", stunSrv.LocalAddr().String())

	relaySrv, err := relay.Start(cfg.RelayAddr, relayTable, log)
	if err != nil {
		log.Error("relay listener failed to bind", "addr", cfg.RelayAddr, "err", err)
		os.Exit(1)
	}
	defer func() { _ = relaySrv.Close() }()
	log.Info("UDP relay listening", "addr", relaySrv.LocalAddr().String(),
		"advertise", cfg.AdvertisedRelay(), "idle", cfg.RelayIdle)
	if !config.HasRoutableHost(cfg.AdvertisedRelay()) {
		// A wildcard listen address is not something a client can dial; without
		// -relay-advertise every RelayAllocated would hand out a dead address.
		log.Warn("advertised relay_addr has no routable host — set -relay-advertise to the public host:port",
			"advertise", cfg.AdvertisedRelay())
	}

	ws := wsapi.NewServer(mgr, cfg, log)
	srv := &http.Server{
		Addr:    cfg.WSAddr,
		Handler: ws.Handler(),
		// ReadHeaderTimeout + MaxHeaderBytes bound a slow or bloated upgrade
		// request; IdleTimeout reclaims keep-alive sockets that never make
		// another request. Deliberately NO ReadTimeout/WriteTimeout: those are
		// whole-request deadlines, and a WebSocket request lasts as long as the
		// lobby does.
		ReadHeaderTimeout: 10 * time.Second,
		IdleTimeout:       60 * time.Second,
		MaxHeaderBytes:    wsapi.MaxHeaderBytes,
	}
	log.Info("control-plane limits",
		"max_conns", cfg.MaxConns, "max_conns_per_ip", cfg.MaxConnsPerIP,
		"max_lobbies", cfg.MaxLobbies, "conn_idle_timeout", cfg.ConnIdleTimeout,
		"client_ip_header", cfg.ClientIPHeader)
	if cfg.ClientIPHeader == "" {
		// Behind an edge proxy every connection arrives from a private address,
		// so ratelimit.SourceKey declines to cap it and -max-conns-per-ip does
		// nothing. Say so at startup rather than letting an operator assume a
		// live cap.
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
