// Command gateway is a reference Go API layer in front of C++ fpx-server
// matcher shards (see docs/SCALING.md).
//
//   - Enrolment / delete / verify are routed to one shard by hash(id).
//   - Identification fans out to every shard concurrently and merges top-k,
//     so latency ~= the slowest shard, not the sum.
//   - Auth, validation, rate limiting, audit logs and customer data belong
//     here, in Go; the C++ nodes only hold templates in RAM and match.
//
// Standard library only:   go run . -shards http://m1:8080,http://m2:8080
package main

import (
	"bytes"
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"hash/fnv"
	"io"
	"log"
	"net/http"
	"net/url"
	"sort"
	"strings"
	"sync"
	"time"
)

type gateway struct {
	shards    []string
	shardKey  string // X-API-Key sent to shards
	clientKey string // X-API-Key required from clients ("" = none)
	http      *http.Client
	threshold float64
}

type candidate struct {
	ID             string            `json:"id"`
	Score          float64           `json:"score"` // fused score (sum over fingers)
	Match          bool              `json:"match"`
	FingersMatched int               `json:"fingers_matched"`
	Fingers        []json.RawMessage `json:"fingers,omitempty"`
}

type identifyResp struct {
	Candidates []candidate `json:"candidates"`
	Compared   uint64      `json:"compared"`
	ElapsedMS  float64     `json:"elapsed_ms"`
}

func (g *gateway) shardFor(id string) string {
	h := fnv.New32a()
	h.Write([]byte(id))
	return g.shards[int(h.Sum32())%len(g.shards)]
}

// forward sends body to one shard and returns status + raw response.
func (g *gateway) forward(ctx context.Context, method, url string, body []byte) (int, []byte, error) {
	req, err := http.NewRequestWithContext(ctx, method, url, bytes.NewReader(body))
	if err != nil {
		return 0, nil, err
	}
	req.Header.Set("Content-Type", "application/json")
	if g.shardKey != "" {
		req.Header.Set("X-API-Key", g.shardKey)
	}
	resp, err := g.http.Do(req)
	if err != nil {
		return 0, nil, err
	}
	defer resp.Body.Close()
	b, err := io.ReadAll(resp.Body)
	return resp.StatusCode, b, err
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func (g *gateway) auth(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if g.clientKey != "" && r.Header.Get("X-API-Key") != g.clientKey {
			writeJSON(w, 401, map[string]string{"error": "unauthorized"})
			return
		}
		next(w, r)
	}
}

// /v1/subjects/{id}[/fingers/{pos}] (GET|PUT|POST|DELETE) -> owning shard.
// Routing uses the subject id only, so all fingers of a customer live on the
// same shard and multi-finger fusion works there.
func (g *gateway) subjects(w http.ResponseWriter, r *http.Request) {
	rest := strings.TrimPrefix(r.URL.EscapedPath(), "/v1/subjects/")
	parts := strings.Split(rest, "/")
	valid := parts[0] != "" && (len(parts) == 1 || (len(parts) == 3 && parts[1] == "fingers" && parts[2] != ""))
	if !valid {
		writeJSON(w, 400, map[string]string{"error": "expected /v1/subjects/{id} or /v1/subjects/{id}/fingers/{position}"})
		return
	}
	id, err := url.PathUnescape(parts[0])
	if err != nil {
		writeJSON(w, 400, map[string]string{"error": "bad id"})
		return
	}
	body, _ := io.ReadAll(io.LimitReader(r.Body, 20<<20))
	status, out, err := g.forward(r.Context(), r.Method, g.shardFor(id)+"/v1/subjects/"+rest, body)
	if err != nil {
		writeJSON(w, 502, map[string]string{"error": err.Error()})
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	w.Write(out)
}

// POST /v1/verify {"id": ...} -> owning shard.
func (g *gateway) verify(w http.ResponseWriter, r *http.Request) {
	body, _ := io.ReadAll(io.LimitReader(r.Body, 20<<20))
	var req struct {
		ID string `json:"id"`
	}
	if json.Unmarshal(body, &req) != nil || req.ID == "" {
		writeJSON(w, 400, map[string]string{"error": "'id' required"})
		return
	}
	status, out, err := g.forward(r.Context(), "POST", g.shardFor(req.ID)+"/v1/verify", body)
	if err != nil {
		writeJSON(w, 502, map[string]string{"error": err.Error()})
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	w.Write(out)
}

// POST /v1/identify -> all shards in parallel, merge top-k.
func (g *gateway) identify(w http.ResponseWriter, r *http.Request) {
	start := time.Now()
	body, _ := io.ReadAll(io.LimitReader(r.Body, 20<<20))
	var req struct {
		TopK int `json:"top_k"`
	}
	_ = json.Unmarshal(body, &req)
	if req.TopK <= 0 {
		req.TopK = 5
	}
	ctx, cancel := context.WithTimeout(r.Context(), 60*time.Second)
	defer cancel()

	var (
		mu       sync.Mutex
		all      []candidate
		compared uint64
		errs     []string
		wg       sync.WaitGroup
	)
	for _, s := range g.shards {
		wg.Add(1)
		go func(shard string) {
			defer wg.Done()
			status, out, err := g.forward(ctx, "POST", shard+"/v1/identify", body)
			mu.Lock()
			defer mu.Unlock()
			if err != nil || status != 200 {
				errs = append(errs, fmt.Sprintf("%s: status %d %v %s", shard, status, err, out))
				return
			}
			var res identifyResp
			if err := json.Unmarshal(out, &res); err != nil {
				errs = append(errs, shard+": "+err.Error())
				return
			}
			all = append(all, res.Candidates...)
			compared += res.Compared
		}(s)
	}
	wg.Wait()
	if len(errs) == len(g.shards) {
		writeJSON(w, 502, map[string]any{"error": "all shards failed", "details": errs})
		return
	}
	// Same ordering as a single shard: accepted matches first, then fused score.
	sort.Slice(all, func(i, j int) bool {
		if all[i].Match != all[j].Match {
			return all[i].Match
		}
		return all[i].Score > all[j].Score
	})
	if len(all) > req.TopK {
		all = all[:req.TopK]
	}
	resp := map[string]any{
		"candidates": all,
		"compared":   compared,
		"elapsed_ms": float64(time.Since(start).Microseconds()) / 1000,
		"match":      len(all) > 0 && all[0].Match,
		"partial":    len(errs) > 0, // some shard failed: result may be incomplete
	}
	if len(all) > 0 && all[0].Match {
		resp["id"] = all[0].ID
	}
	if len(errs) > 0 {
		resp["shard_errors"] = errs
	}
	writeJSON(w, 200, resp)
}

func main() {
	shards := flag.String("shards", "http://localhost:8080", "comma-separated fpx-server base URLs")
	listen := flag.String("listen", ":9090", "listen address")
	shardKey := flag.String("shard-key", "", "X-API-Key for the shards")
	clientKey := flag.String("api-key", "", "X-API-Key required from clients")
	flag.Parse()

	g := &gateway{
		shards:    strings.Split(*shards, ","),
		shardKey:  *shardKey,
		clientKey: *clientKey,
		http: &http.Client{Timeout: 90 * time.Second, Transport: &http.Transport{
			MaxIdleConnsPerHost: 64, IdleConnTimeout: 90 * time.Second}},
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, map[string]any{"status": "ok", "shards": len(g.shards)})
	})
	mux.HandleFunc("/v1/subjects/", g.auth(g.subjects))
	mux.HandleFunc("/v1/verify", g.auth(g.verify))
	mux.HandleFunc("/v1/identify", g.auth(g.identify))
	log.Printf("gateway on %s -> %d shard(s): %s", *listen, len(g.shards), *shards)
	log.Fatal(http.ListenAndServe(*listen, mux))
}
