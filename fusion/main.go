// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 CEMAXECUTER LLC
//
// meshtastic-fusion: central aggregator for one-or-more meshtastic-sniffer
// stations. Subscribes to N sniffer ZMQ PUB feeds, groups same-packet
// observations across stations by (from, packet_id) within a time window,
// and prints / serves a consolidated view.
//
// Sniffer-side (each station) emits JSON events tagged with `station`,
// `station_lat`, `station_lon`, `station_alt_m` (when --gpsd is running)
// over ZMQ PUB. Fusion connects to N of those endpoints and presents the
// "where each station heard each packet" picture in one place.
//
// This file is the v0 CLI subscriber. Web dashboard, sensor management
// API, and C2 fan-out land in subsequent commits behind their own flags
// so the binary stays usable at every step.

package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"sort"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/go-zeromq/zmq4"
)

// Frame is the subset of sniffer event JSON fields we care about for
// multi-station correlation. Anything else passes through unparsed.
type Frame struct {
	Event       string  `json:"event,omitempty"`
	Station     string  `json:"station,omitempty"`
	StationLat  float64 `json:"station_lat,omitempty"`
	StationLon  float64 `json:"station_lon,omitempty"`
	From        string  `json:"from,omitempty"`
	PacketID    uint32  `json:"packet_id,omitempty"`
	Channel     uint8   `json:"channel,omitempty"`
	ChannelName string  `json:"channel_name,omitempty"`
	Preset      string  `json:"preset,omitempty"`
	HopLimit    int     `json:"hop_limit,omitempty"`
	HopStart    int     `json:"hop_start,omitempty"`
	SnrDB       float64 `json:"snr_db,omitempty"`
	RssiDB      float64 `json:"rssi_db,omitempty"`
	Decrypted   *bool   `json:"decrypted,omitempty"`
	PortName    string  `json:"port_name,omitempty"`
}

// Observation is one (station, frame) tuple inside a cluster.
type Observation struct {
	Station    string
	StationLat float64
	StationLon float64
	SnrDB      float64
	RssiDB     float64
	At         time.Time
}

// Cluster groups same-packet observations from one transmission.
type Cluster struct {
	Key          string // "from|packet_id"
	FirstSeen    time.Time
	Frame        Frame // representative (first-seen) frame for non-station fields
	Observations []Observation
}

func clusterKey(from string, pid uint32) string {
	return fmt.Sprintf("%s|%d", from, pid)
}

// subscribe connects a SUB socket to each endpoint and writes received
// raw JSON bytes onto out. The context cancels the goroutines on shutdown.
func subscribe(ctx context.Context, endpoints []string, out chan<- []byte) {
	var wg sync.WaitGroup
	for _, ep := range endpoints {
		wg.Add(1)
		go func(ep string) {
			defer wg.Done()
			s := zmq4.NewSub(ctx)
			defer s.Close()
			s.SetOption(zmq4.OptionSubscribe, "") // wildcard
			if err := s.Dial(ep); err != nil {
				log.Printf("subscribe %s: dial: %v", ep, err)
				return
			}
			log.Printf("subscribed to %s", ep)
			for {
				msg, err := s.Recv()
				if err != nil {
					if ctx.Err() != nil {
						return // shutdown
					}
					log.Printf("subscribe %s: recv: %v", ep, err)
					return
				}
				if len(msg.Frames) == 0 {
					continue
				}
				select {
				case out <- msg.Frames[0]:
				case <-ctx.Done():
					return
				}
			}
		}(ep)
	}
	wg.Wait()
	close(out)
}

// flushReady removes clusters older than `window` from `pending`,
// returning them sorted by first-seen.
func flushReady(pending map[string]*Cluster, window time.Duration, now time.Time) []*Cluster {
	var ready []*Cluster
	for k, c := range pending {
		if now.Sub(c.FirstSeen) > window {
			ready = append(ready, c)
			delete(pending, k)
		}
	}
	sort.Slice(ready, func(i, j int) bool {
		return ready[i].FirstSeen.Before(ready[j].FirstSeen)
	})
	return ready
}

func printCluster(c *Cluster, totalStations int) {
	stations := map[string]bool{}
	parts := make([]string, 0, len(c.Observations))
	for _, o := range c.Observations {
		stations[o.Station] = true
		if o.SnrDB != 0 {
			parts = append(parts, fmt.Sprintf("%s=%.1fdB", o.Station, o.SnrDB))
		} else {
			parts = append(parts, o.Station)
		}
	}
	chName := c.Frame.ChannelName
	if chName == "" {
		chName = "(encrypted)"
	}
	preset := c.Frame.Preset
	if preset == "" {
		preset = "?"
	}
	fmt.Printf("%-11s pid=%-10d %-11s ch=%-12s hop=%d/%d  heard-by=%d/%d [%s]\n",
		c.Frame.From, c.Frame.PacketID, preset, chName,
		c.Frame.HopLimit, c.Frame.HopStart,
		len(stations), totalStations, strings.Join(parts, ", "))
}

func main() {
	window := flag.Duration("window", 5*time.Second,
		"Dedup window across stations (e.g. 3s, 250ms)")
	maxFrames := flag.Int("max-frames", 0,
		"Stop after N consolidated frames (0 = unlimited)")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr,
			"Usage: %s [flags] tcp://host1:7008 tcp://host2:7008 ...\n\n"+
				"Subscribes to N meshtastic-sniffer ZMQ PUB feeds, groups same-packet\n"+
				"observations by (from, packet_id), prints one consolidated line per\n"+
				"real transmission with which stations heard it.\n\nFlags:\n",
			os.Args[0])
		flag.PrintDefaults()
	}
	flag.Parse()
	endpoints := flag.Args()
	if len(endpoints) == 0 {
		flag.Usage()
		os.Exit(2)
	}

	ctx, cancel := context.WithCancel(context.Background())
	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-sigs
		log.Printf("shutting down...")
		cancel()
	}()

	raw := make(chan []byte, 256)
	go subscribe(ctx, endpoints, raw)

	pending := map[string]*Cluster{}
	consolidated := 0
	tick := time.NewTicker(*window / 4)
	defer tick.Stop()

loop:
	for {
		select {
		case b, ok := <-raw:
			if !ok {
				break loop
			}
			var f Frame
			if err := json.Unmarshal(b, &f); err != nil {
				continue
			}
			if f.Event != "" {
				// STATS / OFF_GRID_LORA / REPLAY_SUSPECTED -- not packet data.
				// In a future commit we'll surface these too.
				continue
			}
			if f.From == "" || f.PacketID == 0 {
				continue
			}
			key := clusterKey(f.From, f.PacketID)
			c, ok := pending[key]
			now := time.Now()
			if !ok {
				c = &Cluster{Key: key, FirstSeen: now, Frame: f}
				pending[key] = c
			}
			station := f.Station
			if station == "" {
				station = "(unnamed)"
			}
			c.Observations = append(c.Observations, Observation{
				Station: station, StationLat: f.StationLat, StationLon: f.StationLon,
				SnrDB: f.SnrDB, RssiDB: f.RssiDB, At: now,
			})
		case now := <-tick.C:
			ready := flushReady(pending, *window, now)
			for _, c := range ready {
				printCluster(c, len(endpoints))
				consolidated++
				if *maxFrames > 0 && consolidated >= *maxFrames {
					cancel()
					return
				}
			}
		case <-ctx.Done():
			break loop
		}
	}

	// Final flush of anything still pending past the window.
	for _, c := range flushReady(pending, 0, time.Now()) {
		printCluster(c, len(endpoints))
	}
}
