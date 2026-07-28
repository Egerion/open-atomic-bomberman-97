// Package config resolves the service's runtime configuration from flags with
// environment fallbacks (flags win), and builds the logger. It is the one place
// a default lives, so every construction path — main, tests, anything later —
// sees the same bounds.
package config

import (
	"flag"
	"net"
	"os"
	"strconv"
	"strings"
	"time"

	"go.uber.org/zap"
	"go.uber.org/zap/zapcore"
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
	LogFormat         string        // json|console
	TLSCert           string        // optional cert path for standalone wss://
	TLSKey            string        // optional key path for standalone wss://

	// Capacity caps (SECURITY.md). Everything an unauthenticated stranger can
	// cause the server to allocate is bounded by one of these; 0 selects the
	// default, a negative value disables that cap.
	MaxConns        int           // concurrent WebSocket connections, total
	MaxConnsPerIP   int           // concurrent WebSocket connections from one client IP
	MaxLobbies      int           // live lobbies, total
	ConnIdleTimeout time.Duration // close a connection that holds no seat and says nothing
	ClientIPHeader  string        // trusted edge header naming the real client IP ("" ⇒ off)

	// Relay cost caps. The relay is the one component that spends the operator's
	// money (README "Relay: bandwidth & cost"), and these are the two ceilings on
	// it. Both refuse NEW allocations only — see relay.Limits.
	MaxRelayAllocs int // concurrent relay allocations (2 per relayed match)
	RelayBudgetGB  int // total relay egress budget for this PROCESS, in GB

	// Shutdown (app.Run). DrainDelay is the pause between /readyz going down and
	// the listener closing, so a load balancer has time to stop routing new
	// connections here; ShutdownTimeout bounds the whole drain.
	DrainDelay      time.Duration
	ShutdownTimeout time.Duration
}

// Capacity defaults. Sized so that a legitimate deployment never meets them and
// a stranger cannot walk past them: the whole free-tier instance is expected to
// carry tens of concurrent lobbies, not thousands.
const (
	kDefaultMaxConns        = 2000
	kDefaultMaxConnsPerIP   = 16
	kDefaultMaxLobbies      = 5000
	kDefaultConnIdleTimeout = 120 * time.Second

	// Both relay caps are sized off a MEASURED per-match rate, not a guess. Two
	// real RollbackSessions over a metered transport send exactly 2 datagrams per
	// peer per 20 Hz tick (an InputRange of 8+W bytes and a fixed 13-byte Hash),
	// which for the 2-seat match a relay always carries is ~4.5 KB/s of BILLED
	// egress — 56 B/peer/tick through the forwarder plus the 28 B of IPv4+UDP
	// header the host charges for and this process never sees. That is ~16 MB per
	// relayed match-hour. Latency barely moves it (+1 B/peer/tick per 100 ms of
	// RTT, capped at the 8-tick prediction window) and loss moves it less than
	// half a percent, so the whole plausible range is 16–17 MB/h.
	//
	// kDefaultMaxRelayAllocs is 256 rows = 128 concurrent relayed 2-seat matches
	// (a relayed match is ALWAYS exactly two seats — the client refuses to relay
	// anything bigger). It is chosen from the RATE that many matches imply rather
	// than from the table's memory cost: 128 × 4.5 KB/s ≈ 573 KB/s ≈ 4.6 Mbit/s
	// sustained, which is a credible ceiling for one small machine's NIC, and
	// which would spend the whole default budget below in ~26 hours of full
	// saturation. So the two caps agree rather than overlap: this one bounds the
	// instantaneous rate, the other bounds the total. Observed use is single-digit
	// concurrent lobbies and the relay is only the FALLBACK for those, so 128 sits
	// two orders of magnitude above real traffic. It is well under -max-conns
	// (2000) on purpose: every allocation needs a seated connection, so without a
	// relay-specific cap the connection ceiling would be the only bound and it is
	// far too loose to be one.
	kDefaultMaxRelayAllocs = 256

	// kDefaultRelayBudgetGB is deliberately generous: at the measured ~4.5 KB/s
	// it is ~3300 relayed match-hours, which organic play cannot come near, so it
	// never bites a real player. It is insurance against a runaway bug or
	// deliberate abuse, NOT a monthly quota — the counter lives in RAM and starts
	// again at zero on every deploy, restart or crash. SECURITY.md A7 states
	// exactly what that does and does not promise, and names the Fly spend alert
	// as the backstop no code in this process can be.
	kDefaultRelayBudgetGB = 50

	// kDefaultShutdownTimeout bounds the whole drain. It has to fit inside the
	// platform's own SIGTERM→SIGKILL window (Fly's default is 5 s, extended by
	// kill_timeout in fly.toml), and the work it covers is closing sockets, not
	// finishing requests, so it is short on purpose.
	kDefaultShutdownTimeout = 10 * time.Second

	// kDefaultDrainDelay is 0: with one instance behind the edge, pausing before
	// the listener closes only lengthens the outage. Set it to a couple of the
	// platform's health-check intervals when running more than one instance.
	kDefaultDrainDelay = 0
)

// Log sampling (see NewLogger). Chosen to sit far above anything the service
// emits on purpose — the periodic aggregate lines are one per 10 s each — so the
// sampler only ever fires on a bug.
const (
	kLogSampleFirst      = 100
	kLogSampleThereafter = 100
)

// WithDefaults fills in the caps a zero-valued Config leaves unset, so every
// construction path (main, tests, future callers) gets the same bounds. A
// NEGATIVE value is preserved and means "disabled" — the escape hatch for an
// operator who caps elsewhere.
func (c Config) WithDefaults() Config {
	if c.MaxConns == 0 {
		c.MaxConns = kDefaultMaxConns
	}
	if c.MaxConnsPerIP == 0 {
		c.MaxConnsPerIP = kDefaultMaxConnsPerIP
	}
	if c.MaxLobbies == 0 {
		c.MaxLobbies = kDefaultMaxLobbies
	}
	if c.ConnIdleTimeout == 0 {
		c.ConnIdleTimeout = kDefaultConnIdleTimeout
	}
	if c.MaxRelayAllocs == 0 {
		c.MaxRelayAllocs = kDefaultMaxRelayAllocs
	}
	if c.RelayBudgetGB == 0 {
		c.RelayBudgetGB = kDefaultRelayBudgetGB
	}
	if c.ShutdownTimeout == 0 {
		c.ShutdownTimeout = kDefaultShutdownTimeout
	}
	return c
}

// RelayEgressBudgetBytes converts the operator-facing GB figure into the bytes
// the relay counts. GB is the unit the flag speaks because the number is an
// egress allowance an operator reads off a hosting invoice, and "50" is a value
// somebody can check at a glance where 53687091200 is not. It is a BINARY GB
// (GiB, 1024³) — the larger reading, so the cap is never tighter than the
// operator asked for.
//
// A negative budget (the documented "disabled") stays negative, which relay.Limits
// reads as unlimited. The clamp stops a preposterous value from overflowing into
// a NEGATIVE byte count, which would silently disable the very cap it was set to
// tighten.
func (c Config) RelayEgressBudgetBytes() int64 {
	const gib = int64(1) << 30
	if c.RelayBudgetGB < 0 {
		return -1
	}
	if int64(c.RelayBudgetGB) > (1<<62)/gib {
		return 1 << 62
	}
	return int64(c.RelayBudgetGB) * gib
}

// AdvertisedRelay is the "host:port" put in RelayAllocated. When -relay-advertise
// is unset we fall back to the configured listen address — the same host/port
// pattern the STUN listener already uses (same host as the control plane, its
// own UDP port). A wildcard listen address is not routable, so main.go warns at
// startup when the fallback has no real host.
func (c Config) AdvertisedRelay() string {
	if c.RelayAdvertise != "" {
		return c.RelayAdvertise
	}
	return c.RelayAddr
}

// HasRoutableHost reports whether an advertised "host:port" names a host a
// client could actually reach (i.e. not ":8082", "0.0.0.0:8082" or "[::]:8082").
func HasRoutableHost(addr string) bool {
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

func Parse(args []string) Config {
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
	logFormat := fs.String("log-format", envStr("MATCHMAKER_LOG_FORMAT", "json"), "log encoding: json|console")
	tlsCert := fs.String("tls-cert", envStr("MATCHMAKER_TLS_CERT", ""), "optional TLS cert path for standalone wss:// (else terminate TLS at the edge)")
	tlsKey := fs.String("tls-key", envStr("MATCHMAKER_TLS_KEY", ""), "optional TLS key path for standalone wss://")
	maxConns := fs.Int("max-conns", envInt("MATCHMAKER_MAX_CONNS", 0), "max concurrent WebSocket connections (0 ⇒ default, <0 ⇒ unlimited)")
	maxConnsPerIP := fs.Int("max-conns-per-ip", envInt("MATCHMAKER_MAX_CONNS_PER_IP", 0), "max concurrent WebSocket connections from one client IP (0 ⇒ default, <0 ⇒ unlimited)")
	maxLobbies := fs.Int("max-lobbies", envInt("MATCHMAKER_MAX_LOBBIES", 0), "max live lobbies (0 ⇒ default, <0 ⇒ unlimited)")
	connIdle := fs.Duration("conn-idle-timeout", envDur("MATCHMAKER_CONN_IDLE_TIMEOUT", 0), "close a seatless, silent connection after this long (0 ⇒ default, <0 ⇒ never)")
	maxRelayAllocs := fs.Int("max-relay-allocs", envInt("MATCHMAKER_MAX_RELAY_ALLOCS", 0), "max concurrent relay allocations, 2 per relayed match (0 ⇒ default, <0 ⇒ unlimited)")
	relayBudgetGB := fs.Int("relay-budget-gb", envInt("MATCHMAKER_RELAY_BUDGET_GB", 0), "total relay egress this PROCESS may forward, in GB; resets on restart (0 ⇒ default, <0 ⇒ unlimited)")
	clientIPHeader := fs.String("client-ip-header", envStr("MATCHMAKER_CLIENT_IP_HEADER", ""), "trusted edge header carrying the real client IP, e.g. Fly-Client-IP (empty ⇒ use the socket peer)")
	drainDelay := fs.Duration("drain-delay", envDur("MATCHMAKER_DRAIN_DELAY", kDefaultDrainDelay), "pause between /readyz going down and the listener closing, so a load balancer can stop routing here")
	shutdownTimeout := fs.Duration("shutdown-timeout", envDur("MATCHMAKER_SHUTDOWN_TIMEOUT", 0), "bound on the whole graceful drain (0 ⇒ default)")
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
		LogFormat:         *logFormat,
		TLSCert:           *tlsCert,
		TLSKey:            *tlsKey,
		MaxConns:          *maxConns,
		MaxConnsPerIP:     *maxConnsPerIP,
		MaxLobbies:        *maxLobbies,
		ConnIdleTimeout:   *connIdle,
		ClientIPHeader:    *clientIPHeader,
		MaxRelayAllocs:    *maxRelayAllocs,
		RelayBudgetGB:     *relayBudgetGB,
		DrainDelay:        *drainDelay,
		ShutdownTimeout:   *shutdownTimeout,
	}.WithDefaults()
}

// NewLogger builds the process logger: structured, levelled, JSON by default so
// the deployed instance's output is machine-readable, `console` for a human at a
// terminal.
//
// SAMPLING IS ON DELIBERATELY, and it is the reason zap is here rather than a
// hand-rolled wrapper. Rule 4 of SECURITY.md is that untrusted input must never
// be able to write to the log at its own chosen rate, which today is enforced by
// hand — every refusal is counted and reported in one periodic line. The sampler
// is the structural backstop for the day somebody adds a per-event log line by
// accident: past kLogSampleFirst occurrences of the same message in a second,
// only every kLogSampleThereafter-th one is emitted. Nothing this service logs
// on purpose comes within two orders of magnitude of that.
func NewLogger(level, format string) *zap.Logger {
	cfg := zap.NewProductionConfig()
	cfg.Level = zap.NewAtomicLevelAt(ParseLevel(level))
	cfg.Sampling = &zap.SamplingConfig{Initial: kLogSampleFirst, Thereafter: kLogSampleThereafter}
	// stdout, like the slog handler this replaced: the platform captures both, and
	// keeping the stream stable means existing log pipelines do not move.
	cfg.OutputPaths = []string{"stdout"}
	cfg.ErrorOutputPaths = []string{"stderr"}
	cfg.EncoderConfig.EncodeTime = zapcore.ISO8601TimeEncoder
	// Durations as "1m0s", not as a bare 60. Every duration this service logs is
	// a configured timeout an operator is checking against what they set, and
	// "idle": 60 next to "conn_idle_timeout": 120 is a unit question waiting to
	// be answered wrong.
	cfg.EncoderConfig.EncodeDuration = zapcore.StringDurationEncoder
	if strings.EqualFold(strings.TrimSpace(format), "console") {
		cfg.Encoding = "console"
		cfg.EncoderConfig.EncodeLevel = zapcore.CapitalLevelEncoder
	}
	log, err := cfg.Build()
	if err != nil {
		// Only a malformed encoder config can get here, and running blind is worse
		// than running with zap's own minimal logger.
		return zap.NewExample()
	}
	return log
}

// ParseLevel maps the configured name onto a zap level, defaulting to info for
// anything unrecognised (a typo must not silence the service).
func ParseLevel(level string) zapcore.Level {
	switch strings.ToLower(strings.TrimSpace(level)) {
	case "debug":
		return zapcore.DebugLevel
	case "warn", "warning":
		return zapcore.WarnLevel
	case "error":
		return zapcore.ErrorLevel
	default:
		return zapcore.InfoLevel
	}
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
