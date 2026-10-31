<p align="center">
  <img src="docs/banner.svg" alt="fleet_database — storage of the fms: PostgreSQL behind gRPC" width="100%">
</p>

# fleet_database

**The storage of the fms: PostgreSQL behind a gRPC API.**

Services never talk to each other through the database — they call each other over gRPC. fleet_database keeps what must survive a restart: the robots registered by fleet_control, the robot status and factsheets written by fleet_dispatch, and later missions, maps and the data dispatch needs to recover. The web UI reads from it over REST (read-only).

> **Work in progress.** Robots, their status and factsheets are stored and served over gRPC and REST. Missions, maps and the data dispatch needs to recover after a restart come next.

## One writer per field

Every piece of data has exactly one owner. The API is split by writer, so a service can only change what belongs to it:

| gRPC service | Written by | What |
|---|---|---|
| `RobotRegistry` | fleet_control | robots as entities: id, manufacturer + serial number, name, labels, soft deletion (`PENDING_DELETE`); also serves the stored factsheet |
| `DispatchStorage` | fleet_dispatch | robot status, factsheets, mission execution, `headerId` blocks; a snapshot to recover after a restart |

Any service may read. Contracts: [`proto/fleet_database.proto`](../proto/fleet_database.proto) (package `fms.database.v1`).

## How it works

```mermaid
flowchart LR
    control[fleet_control] -- gRPC --> service
    dispatch[fleet_dispatch] -- gRPC --> service
    ui[web UI] -- "REST, read-only" --> rest

    subgraph fdb [fleet_database]
        service[DatabaseService<br/>RobotRegistry · DispatchStorage]
        rest[RestApi<br/>GET /api/fms/*]
        thread[database thread<br/>one query at a time]
        service -- co_spawn / co_await --> thread
        rest -- co_spawn / co_await --> thread
    end

    thread -- SQL --> pg[(PostgreSQL)]
```

| Part | Responsibility |
|---|---|
| `GrpcServer` | gRPC server with health checking and reflection (shared with fleet_dispatch, from `shared/`) |
| `HttpServer`, `ApiRouter` | HTTP server and route table (shared with fleet_control, from `shared/`) |
| `DatabaseService` | gRPC handlers of both services; transport only — take the request, run the query, answer |
| `RestApi`, `ApiJson` | REST handlers for the UI and the robot in the UI contract format |
| `RobotTable` | SQL of the robot tables: free functions, no gRPC or HTTP inside |
| `Database` | the PostgreSQL connection (libpqxx), migrations, the database thread, reconnect after a lost connection |

**Threads.** libpqxx queries block, so they never run on the gRPC/HTTP thread. The database thread owns the connection and its own `io_context`, which is the queue of queries: a handler hands a query over with `co_spawn(database.context(), ...)` and waits for the result with `co_await`, while the gRPC thread keeps serving other calls. Queries run strictly one after another — no locking between them.

## Migrations

The schema lives in numbered SQL files in [`migrations/`](migrations): `001_robots.sql`, `002_….sql`, … On every start the service applies the files that are not yet recorded in the `schema_migrations` table, in name order, each together with its record in one transaction. A new empty database and an old one both end up with exactly the schema the running code expects.

Rules: an applied file is never edited — every change is a new file with the next number.

| File | Tables |
|---|---|
| `001_robots.sql` | `robots` |
| `002_robot_status.sql` | `robot_status`, `robot_factsheets` — the latest of each, deleted together with the robot |
| next | mission execution, `headerId` counters |

## gRPC API

| RPC | Status |
|---|---|
| `RobotRegistry`: `CreateRobot`, `UpdateRobot`, `SetRobotDeleting`, `DeleteRobot`, `GetRobot`, `ListRobots`, `GetFactsheet` | done |
| `DispatchStorage`: `SaveRobotStatus`, `SaveFactsheet` | done; fleet_dispatch writes them |
| `DispatchStorage`: `SaveMissionExecution`, `ReserveHeaderIds`, `LoadDispatchSnapshot` | planned, together with dispatch recovery |

Errors are gRPC status codes: `NOT_FOUND`, `ALREADY_EXISTS` (busy `robot_id` or manufacturer + serial number), `FAILED_PRECONDITION`, `INVALID_ARGUMENT`, `UNAVAILABLE` (database down — retry later).

## REST API (read-only, for the UI)

The format is the UI contract (`docs/API_CONTRACT.md` in the UI repository, section 5): `camelCase`, enums without the proto prefix (`"ONLINE"`), time in ISO 8601 UTC with milliseconds, a missing value is `null`. Errors: `{"code": "NOT_FOUND", "message": "...", "details": {}}`.

| Path | Answer |
|---|---|
| `GET /api/fms/robots` | `Robot[]`, including robots being deleted (`lifecycle: PENDING_DELETE`) |
| `GET /api/fms/robots/{id}` | `Robot`; 404 `NOT_FOUND` |
| `GET /api/fms/robots/{id}/factsheet` | the factsheet as the robot sent it (VDA 5050 JSON); 404 if none yet |

`availability`, `graphPosition` (computed by fleet_control) and `info` (factsheet summary) are `null` for now.

There is no authentication in the service: the gateway checks the JWT. Do not expose the HTTP port directly — only behind the gateway.

## Configuration

Environment variables, with defaults for development:

| Variable | Default | What |
|---|---|---|
| `DATABASE_URL` | `postgresql://fms:fms@127.0.0.1:5432/fms` | PostgreSQL connection |
| `GRPC_ADDRESS` | `0.0.0.0:5002` | gRPC listen address |
| `HTTP_ADDRESS` | `0.0.0.0:8002` | REST listen address |
| `MIGRATIONS_DIR` | `fleet_database/migrations` in the source tree | folder with the SQL migrations |

The connection string is never written to the log (it contains the password).

## Building and running

**PostgreSQL for development** (Ubuntu / WSL):

```bash
sudo apt install postgresql
```

```bash
sudo service postgresql start
```

```bash
sudo -u postgres psql -c "CREATE USER fms WITH PASSWORD 'fms';" -c "CREATE DATABASE fms OWNER fms;"
```

**Build** from the repository root (see the [main README](../README.md#building)); vcpkg builds `libpq`, which needs `bison` and `flex`:

```bash
cmake --build build --target fleet_database -j
```

**Run:**

```bash
./build/fleet_database/fleet_database
```

On the first start the log shows `migration 001_robots.sql applied`; on later starts nothing is applied. Stop with Ctrl+C. If the database is unreachable, the service exits at once with the connection error.

### Check it

```bash
psql postgresql://fms:fms@127.0.0.1:5432/fms -c '\dt'
```

```bash
grpcurl -plaintext localhost:5002 list
```

```bash
grpcurl -plaintext -d '{}' localhost:5002 fms.database.v1.RobotRegistry/ListRobots
```

```bash
curl -s -w '\n' http://localhost:8002/api/fms/robots
```

Automated tests: [`tests/`](tests) (pytest; gRPC and REST).

grpcurl cannot call the health service through reflection (the C++ health service does not publish its descriptor); give it the proto instead, or check it with any gRPC health probe.

## Layout

```
fleet_database/
├── fdb_main.cpp             wiring, start and shutdown order
├── include/                 headers
├── src/
│   ├── Database.cpp         connection, migrations, database thread, reconnect
│   ├── DatabaseService.cpp  gRPC handlers of RobotRegistry and DispatchStorage
│   ├── RestApi.cpp          REST handlers for the UI
│   ├── ApiJson.cpp          robot in the UI contract format
│   └── RobotTable.cpp       SQL of robots, robot_status, robot_factsheets
├── migrations/              numbered SQL files
└── tests/                   pytest: gRPC and REST
```
