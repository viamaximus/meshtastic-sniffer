// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 CEMAXECUTER LLC
//
// fusion/c2.go: command fan-out from the fusion dashboard to every
// registered sensor's HTTP /api/* endpoint.
//
// When the operator clicks "Add key" in the fusion UI, the new key
// should land on every sensor in one shot. The wire-level transport
// is just HTTP POST to each sensor's existing /api/<endpoint>, with
// the optional bearer token from the registry entry. Per-sensor
// success/failure is collected and returned as JSON so the dashboard
// can show partial-failure states.

package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"sync"
	"time"
)

// FanoutResult is one sensor's reply to a fan-out command.
type FanoutResult struct {
	Sensor string `json:"sensor"`
	Status int    `json:"status"`
	Body   string `json:"body"`
	Error  string `json:"error,omitempty"`
}

// fanoutClient is shared across all fan-out calls. 5s per-request
// timeout: command APIs return fast, anything longer is a sign the
// sensor is hung and we should move on.
var fanoutClient = &http.Client{Timeout: 5 * time.Second}

// fanoutCommand POSTs `body` to <sensor.Api>/api/<path> for every
// sensor in the registry that has a non-empty Api endpoint. Adds the
// sensor's ApiToken as a Bearer header when set. Calls run in
// parallel; results come back in registry order.
func fanoutCommand(registry *Registry, path string, body []byte) []FanoutResult {
	sensors := registry.List()
	results := make([]FanoutResult, len(sensors))

	var wg sync.WaitGroup
	for i := range sensors {
		s := sensors[i]
		if s.Api == "" {
			results[i] = FanoutResult{Sensor: s.Name, Error: "no api endpoint configured"}
			continue
		}
		wg.Add(1)
		go func(idx int, s Sensor) {
			defer wg.Done()
			url := s.Api + "/api/" + path
			req, err := http.NewRequest(http.MethodPost, url, bytes.NewReader(body))
			if err != nil {
				results[idx] = FanoutResult{Sensor: s.Name, Error: err.Error()}
				return
			}
			req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
			if s.ApiToken != "" {
				req.Header.Set("Authorization", "Bearer "+s.ApiToken)
			}
			resp, err := fanoutClient.Do(req)
			if err != nil {
				results[idx] = FanoutResult{Sensor: s.Name, Error: err.Error()}
				return
			}
			defer resp.Body.Close()
			b, _ := io.ReadAll(resp.Body)
			results[idx] = FanoutResult{
				Sensor: s.Name, Status: resp.StatusCode, Body: string(b),
			}
		}(i, s)
	}
	wg.Wait()
	return results
}

// installFanoutHandlers registers /api/fanout/<endpoint> handlers on
// the given mux. Each one reads the request body, calls fanoutCommand
// against the registry's current sensor list, and replies with a
// per-sensor JSON array.
func installFanoutHandlers(mux *http.ServeMux, registry *Registry) {
	endpoints := []string{"keys", "share-url", "extra-freq", "cot-multicast"}
	for _, ep := range endpoints {
		ep := ep // capture
		mux.HandleFunc("/api/fanout/"+ep, func(w http.ResponseWriter, r *http.Request) {
			if r.Method != http.MethodPost {
				http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
				return
			}
			body, err := io.ReadAll(r.Body)
			if err != nil {
				http.Error(w, fmt.Sprintf(`{"error":"read body: %s"}`, err),
					http.StatusBadRequest)
				return
			}
			results := fanoutCommand(registry, ep, body)
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(map[string]any{
				"endpoint": ep,
				"results":  results,
			})
		})
	}
}
