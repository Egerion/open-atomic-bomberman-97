package main

import (
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
)

// crockford is the Crockford base-32 alphabet (RFC-less de-facto standard):
// digits 0-9 then A-Z with I, L, O, U removed — 32 symbols, unambiguous when
// spoken or typed. Lobby codes are 6 of these (~1.07e9 space, design §1.1).
const crockford = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

// newLobbyCode returns a fresh 6-char Crockford base-32 lobby code drawn from
// crypto/rand. Codes are unpredictable (never sequential — ADR-0011 / task
// constraint 3). 32 divides 256, so byte&0x1f is an unbiased symbol index.
func newLobbyCode() (string, error) {
	var b [6]byte
	if _, err := rand.Read(b[:]); err != nil {
		return "", err
	}
	out := make([]byte, len(b))
	for i, v := range b {
		out[i] = crockford[v&0x1f]
	}
	return string(out), nil
}

// newHandle returns a 128-bit opaque handle (32 lowercase hex chars) for a
// lobby_id or host_token — unguessable, from crypto/rand.
func newHandle() (string, error) {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		return "", err
	}
	return hex.EncodeToString(b[:]), nil
}

// newSeed returns a cryptographically-random uint32 match seed (StartMatch,
// design §1.6). The value itself is not security-sensitive, but sourcing it
// from crypto/rand keeps a single randomness path and avoids seeding math/rand.
func newSeed() (uint32, error) {
	var b [4]byte
	if _, err := rand.Read(b[:]); err != nil {
		return 0, err
	}
	return binary.LittleEndian.Uint32(b[:]), nil
}
