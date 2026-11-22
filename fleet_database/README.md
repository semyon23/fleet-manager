<p align="center">
  <img src="docs/banner.svg" alt="COMPASS Fleet Manager Server — Fleet data storage" width="100%">
</p>

# Fleet data storage · `fleet_database`

Stores maps, robot registrations, missions, and their outcomes. The operator interface reads fleet status and event history from this service. Command records let other services check which orders and movement permissions they issued before a restart.

## Build

Run the commands from the repository root. For the first build, set `VCPKG_ROOT` to your vcpkg installation and configure the build directory:

```bash
cmake -S . -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
```

Once configured, build the service:

```bash
cmake --build cmake-build-debug --target fleet_database -j 4
```

## Run

Start PostgreSQL. On a new local test machine, create the role and database if they do not already exist:

```bash
sudo systemctl start postgresql
sudo -u postgres psql -c "CREATE USER fms WITH PASSWORD 'fms';"
sudo -u postgres createdb -O fms fms
```

Start the service against that database:

```bash
DATABASE_URL=postgresql://fms:fms@127.0.0.1:5432/fms \
  ./cmake-build-debug/fleet_database/fleet_database
```

The service applies migrations at startup. Other services connect on port `5002`. To check the HTTP endpoint, list the maps from a second terminal:

```bash
curl -fsS http://127.0.0.1:8002/api/fms/maps
```

A new database returns an empty map list.

To run all four services together, see the [test stand guide](../deploy/README.md).
