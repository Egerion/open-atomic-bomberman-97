package main

import (
	"encoding/json"
	"log/slog"
	"net"
)

// STUN-style reflexive-address echo (design §2). A client sends StunProbe from
// the SAME UDP socket it will punch/match on; the server replies with the
// source ip:port it observed — that socket's public reflexive address as seen
// from outside the NAT. This is the "learn the address off the wire" trick
// UdpTransport::poll() already does, promoted to a server.
//
// Datagram format is one JSON object per UDP packet (documented in PROTOCOL.md
// so the C++ side matches):
//
//	C→S  {"type":"StunProbe","nonce":"<opaque echo token>"}
//	S→C  {"type":"StunReply","nonce":"<same>","your_addr":"81.2.3.4:52001"}
//
// nonce is an opaque STRING echoed verbatim (no server interpretation, no JSON
// number precision pitfalls). Malformed or non-probe datagrams are ignored.

type stunProbe struct {
	Type  string `json:"type"`
	Nonce string `json:"nonce"`
}

type stunReply struct {
	Type     string `json:"type"`
	Nonce    string `json:"nonce"`
	YourAddr string `json:"your_addr"`
}

type stunServer struct {
	conn *net.UDPConn
	log  *slog.Logger
}

func startStun(addr string, log *slog.Logger) (*stunServer, error) {
	udpAddr, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		return nil, err
	}
	conn, err := net.ListenUDP("udp", udpAddr)
	if err != nil {
		return nil, err
	}
	s := &stunServer{conn: conn, log: log}
	go s.serve()
	return s, nil
}

func (s *stunServer) serve() {
	buf := make([]byte, 2048)
	for {
		n, src, err := s.conn.ReadFromUDP(buf)
		if err != nil {
			return // socket closed on shutdown
		}
		var p stunProbe
		if err := json.Unmarshal(buf[:n], &p); err != nil || p.Type != "StunProbe" {
			continue
		}
		reply, err := json.Marshal(stunReply{Type: "StunReply", Nonce: p.Nonce, YourAddr: src.String()})
		if err != nil {
			continue
		}
		if _, err := s.conn.WriteToUDP(reply, src); err != nil {
			s.log.Debug("stun reply failed", "dst", src.String(), "err", err)
		}
	}
}

func (s *stunServer) LocalAddr() net.Addr { return s.conn.LocalAddr() }

func (s *stunServer) Close() error { return s.conn.Close() }
