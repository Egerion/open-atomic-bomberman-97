package config

import (
	"testing"
	"time"
)

func TestRelayAdvertiseFallsBackToListenAddr(t *testing.T) {
	cfg := Parse([]string{"-relay-addr", "127.0.0.1:9999"})
	if got := cfg.AdvertisedRelay(); got != "127.0.0.1:9999" {
		t.Fatalf("unset -relay-advertise should fall back to -relay-addr, got %q", got)
	}
	cfg = Parse([]string{"-relay-addr", ":8082", "-relay-advertise", "relay.example:8082"})
	if got := cfg.AdvertisedRelay(); got != "relay.example:8082" {
		t.Fatalf("-relay-advertise should win, got %q", got)
	}
	if cfg.RelayIdle != 60*time.Second {
		t.Fatalf("default relay idle should be 60s, got %v", cfg.RelayIdle)
	}
}

func TestHasRoutableHost(t *testing.T) {
	for _, c := range []struct {
		addr string
		want bool
	}{
		{"relay.example:8082", true},
		{"203.0.113.7:8082", true},
		{"[2001:db8::1]:8082", true},
		{":8082", false},
		{"0.0.0.0:8082", false},
		{"[::]:8082", false},
		{"nonsense", false},
	} {
		if got := HasRoutableHost(c.addr); got != c.want {
			t.Fatalf("HasRoutableHost(%q) = %v, want %v", c.addr, got, c.want)
		}
	}
}

// The capacity caps must have a default nobody has to remember to set, and a
// negative value must stay negative — that is the "disabled" escape hatch.
func TestCapacityDefaultsAndDisabling(t *testing.T) {
	cfg := Parse(nil)
	if cfg.MaxConns != kDefaultMaxConns || cfg.MaxConnsPerIP != kDefaultMaxConnsPerIP ||
		cfg.MaxLobbies != kDefaultMaxLobbies || cfg.ConnIdleTimeout != kDefaultConnIdleTimeout {
		t.Fatalf("unset caps should take their defaults, got %+v", cfg)
	}
	off := Parse([]string{"-max-conns", "-1", "-conn-idle-timeout", "-1s"})
	if off.MaxConns != -1 || off.ConnIdleTimeout != -time.Second {
		t.Fatalf("a negative cap must survive WithDefaults, got %+v", off)
	}
	explicit := Parse([]string{"-max-lobbies", "7"})
	if explicit.MaxLobbies != 7 {
		t.Fatalf("an explicit cap must win, got %d", explicit.MaxLobbies)
	}
}
