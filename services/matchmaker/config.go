package main

import (
	"flag"
	"log/slog"
	"net"
	"os"
	"strconv"
	"strings"
	"time"
)

// Config is the runtime configuration, resolved from flags with environment
// fallbacks (flags win). See README.md for the full list.
type Config struct {
	WSAddr            string        // WebSocket listen address (":8080")
	StunAddr          string        // UDP STUN echo listen address (":8081")
	RelayAddr         string        // UDP relay forwarder listen address (":8082")
	RelayAdvertise    string        // public host:port handed to clients (empty ⇒ RelayAddr)
	RelayIdle         time.Duration // drop an allocation after this long without traffic
	HeartbeatInterval time.Duration // expected client heartbeat cadence
	HeartbeatMiss     int           // K: missed heartbeats before a member is dropped
	LockedGrace       time.Duration // LOCKED → IN_PROGRESS timeout after StartMatch
	LogLevel          string        // debug|info|warn|error
	TLSCert           string        // optional cert path for standalone wss://
	TLSKey            string        // optional key path for standalone wss://
}

// relayAdvertise is the "host:port" put in RelayAllocated. When -relay-advertise
// is unset we fall back to the configured listen address — the same host/port
// pattern the STUN listener already uses (same host as the control plane, its
// own UDP port). A wildcard listen address is not routable, so main.go warns at
// startup when the fallback has no real host.
func (c Config) relayAdvertise() string {
	if c.RelayAdvertise != "" {
		return c.RelayAdvertise
	}
	return c.RelayAddr
}

// hasRoutableHost reports whether an advertised "host:port" names a host a
// client could actually reach (i.e. not ":8082", "0.0.0.0:8082" or "[::]:8082").
func hasRoutableHost(addr string) bool {
	host, _, err := net.SplitHostPort(addr)
	if err != nil {
		return false
	}
	switch host {
	case "", "0.0.0.0", "::":
		return false
	}
	return true
}

func parseConfig(args []string) Config {
	fs := flag.NewFlagSet("matchmaker", flag.ExitOnError)
	wsAddr := fs.String("ws-addr", envStr("MATCHMAKER_WS_ADDR", ":8080"), "WebSocket control-plane listen address")
	stunAddr := fs.String("stun-addr", envStr("MATCHMAKER_STUN_ADDR", ":8081"), "UDP STUN-echo listen address")
	relayAddr := fs.String("relay-addr", envStr("MATCHMAKER_RELAY_ADDR", ":8082"), "UDP relay-forwarder listen address")
	relayAdvertise := fs.String("relay-advertise", envStr("MATCHMAKER_RELAY_ADVERTISE", ""), "public host:port of the relay advertised to clients (default: -relay-addr)")
	relayIdle := fs.Duration("relay-idle", envDur("MATCHMAKER_RELAY_IDLE", 60*time.Second), "drop a relay allocation after this long without traffic (0 disables)")
	hbInterval := fs.Duration("heartbeat-interval", envDur("MATCHMAKER_HEARTBEAT_INTERVAL", 10*time.Second), "expected client heartbeat cadence")
	hbMiss := fs.Int("heartbeat-miss", envInt("MATCHMAKER_HEARTBEAT_MISS", 3), "missed heartbeats (K) before dropping a member")
	lockedGrace := fs.Duration("locked-grace", envDur("MATCHMAKER_LOCKED_GRACE", 10*time.Second), "grace after StartMatch before the lobby goes IN_PROGRESS")
	logLevel := fs.String("log-level", envStr("MATCHMAKER_LOG_LEVEL", "info"), "log level: debug|info|warn|error")
	tlsCert := fs.String("tls-cert", envStr("MATCHMAKER_TLS_CERT", ""), "optional TLS cert path for standalone wss:// (else terminate TLS at the edge)")
	tlsKey := fs.String("tls-key", envStr("MATCHMAKER_TLS_KEY", ""), "optional TLS key path for standalone wss://")
	_ = fs.Parse(args)

	return Config{
		WSAddr:            *wsAddr,
		StunAddr:          *stunAddr,
		RelayAddr:         *relayAddr,
		RelayAdvertise:    *relayAdvertise,
		RelayIdle:         *relayIdle,
		HeartbeatInterval: *hbInterval,
		HeartbeatMiss:     *hbMiss,
		LockedGrace:       *lockedGrace,
		LogLevel:          *logLevel,
		TLSCert:           *tlsCert,
		TLSKey:            *tlsKey,
	}
}

func newLogger(level string) *slog.Logger {
	var lv slog.Level
	switch strings.ToLower(level) {
	case "debug":
		lv = slog.LevelDebug
	case "warn":
		lv = slog.LevelWarn
	case "error":
		lv = slog.LevelError
	default:
		lv = slog.LevelInfo
	}
	return slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: lv}))
}

func envStr(key, def string) string {
	if v, ok := os.LookupEnv(key); ok {
		return v
	}
	return def
}

func envInt(key string, def int) int {
	if v, ok := os.LookupEnv(key); ok {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return def
}

func envDur(key string, def time.Duration) time.Duration {
	if v, ok := os.LookupEnv(key); ok {
		if d, err := time.ParseDuration(v); err == nil {
			return d
		}
	}
	return def
}
