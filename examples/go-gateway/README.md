# Go gateway (reference)

A thin Go API layer in front of several `fpx-server` matcher shards. Business
logic (auth, customer records, audit, rate limits) lives here; the C++ shards
only hold templates in RAM and match.

```bash
# two shards + gateway on one machine
../../build/fpx-server --db shard0.fpxdb --port 8081 &
../../build/fpx-server --db shard1.fpxdb --port 8082 &
go run . -shards http://localhost:8081,http://localhost:8082 -listen :9090
```

The gateway has the same endpoints as a single `fpx-server` (`PUT /v1/subjects/{id}`,
`POST /v1/identify`, `POST /v1/verify`), so clients don't change when you
go from one node to many. See `docs/SCALING.md`.
