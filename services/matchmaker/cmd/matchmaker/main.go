// Command matchmaker is Open Bomberman's online-multiplayer signaling / lobby
// control plane, STUN echo, and UDP relay forwarder (ADR-0011, design §§1–2,
// §4, §5.2). It does NOT simulate and never sees game State — it brokers lobby
// state, echoes reflexive addresses, and forwards OPAQUE relayed datagrams for
// peers whose hole-punch failed.
//
// This file is the process boundary and nothing else: flags in, a logger, a
// signal-scoped context, an exit code. The object graph and the shutdown order
// live in internal/app, where they can be tested.
package main

import (
	"context"
	"os"
	"os/signal"
	"syscall"

	"go.uber.org/zap"

	"github.com/egedemirbas/open-bomberman/matchmaker/internal/app"
	"github.com/egedemirbas/open-bomberman/matchmaker/internal/config"
)

func main() {
	cfg := config.Parse(os.Args[1:])
	log := config.NewLogger(cfg.LogLevel, cfg.LogFormat)
	defer func() { _ = log.Sync() }()

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	a, err := app.New(cfg, log)
	if err != nil {
		log.Error("startup failed", zap.Error(err))
		_ = log.Sync()
		os.Exit(1)
	}
	if err := a.Run(ctx); err != nil {
		log.Error("stopped with an error", zap.Error(err))
		_ = log.Sync()
		os.Exit(1)
	}
}
