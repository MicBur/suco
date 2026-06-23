# ag-grid-build

`ag-grid-build` ist ein extrem schnelles, verteiltes C/C++ Kompilierungs- und Caching-System, das speziell für Netzwerke mit Kubernetes (k3s) und Redis entwickelt wurde.

Es kombiniert die Stärken von verteiltem Kompilieren (ähnlich wie `icecc`/`distcc`) mit intelligentem, extrem schnellem Caching (ähnlich wie `ccache`) über eine lokale Redis-Replica.

## Features

- **Blendschnelles Caching:** Fragt eine lokale Redis-Replica ab. Bei einem Cache-Hit wird das fertige `.o`-File direkt geladen – Kompilierungszeit nahezu 0 ms.
- **k3s-Integration:** Der Compile-Daemon (`ag-helper`) läuft als DaemonSet in deinem Kubernetes-Cluster. k3s übernimmt das TCP-Load-Balancing vollautomatisch.
- **Isolierte Build-Umgebungen:** Da der Daemon im Docker-Container läuft, müssen auf den Rechnern des Clusters keine Compiler oder Bibliotheken installiert werden.
- **Resilienter Fallback:** Falls das k3s-Cluster oder Redis nicht erreichbar sind, kompiliert der Client den Code vollautomatisch lokal auf der Entwickler-Maschine, ohne den Build zu unterbrechen.

---

## Architektur

1. **Client (`ag-build`):** Präprozessiert Quelldateien lokal (`g++ -E`) und hasht den Inhalt + Flags (SHA-256).
2. **Cache:** Sucht den Hash in der lokalen Redis-Replica.
3. **Helper (`ag-helper`):** Empfängt bei Cache-Miss die preprocessed `.ii`-Datei über k3s, kompiliert diese im Container und liefert das `.o`-File zurück.
4. **Cache-Speicherung:** Der Client schreibt das Resultat in den Redis Master, welcher es asynchron an die lokalen Replicas verteilt.

---

## Installation & Build

### 1. Abhängigkeiten installieren (Ubuntu/Debian)

```bash
sudo apt update
sudo apt install -y build-essential cmake libssl-dev libhiredis-dev
```

### 2. Projekt bauen

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Dies erzeugt zwei Binaries in `build/`:
- `ag-build` (Der Client/Wrapper)
- `ag-helper` (Der Server/Daemon)

---

## Kubernetes (k3s) Deployment

### 1. Redis Master bereitstellen
```bash
kubectl apply -f k8s/redis-master.yaml
```

### 2. Docker-Image bauen und pushen
Baue das Image und pushe es in deine lokale Registry (z. B. auf Port 5000):
```bash
docker build -t local-registry:5000/ag-helper:latest .
docker push local-registry:5000/ag-helper:latest
```

### 3. DaemonSet deployen
```bash
kubectl apply -f k8s/helper-daemonset.yaml
```

---

## Benutzung

Nutze `ag-build` als Präfix für deine Compiler-Aufrufe.

**Beispiel:**
```bash
./build/ag-build g++ -O3 -std=c++17 -c main.cpp -o main.o
```

### Integration in Makefiles
Ersetze einfach den Compiler:
```makefile
CXX = /pfad/zu/ag-build g++
```

### Konfiguration (Umgebungsvariablen)

Folgende Variablen können zur Konfiguration gesetzt werden:

| Variable | Beschreibung | Standardwert |
| :--- | :--- | :--- |
| `AG_HELPER_HOST` | Hostname/IP des k3s-Services oder Helper-Daemons | `127.0.0.1` |
| `AG_HELPER_PORT` | Port des Helper-Daemons | `9000` |
| `AG_REDIS_REPLICA_HOST` | Hostname/IP der lokalen Redis-Replica (Read) | `127.0.0.1` |
| `AG_REDIS_REPLICA_PORT` | Port der lokalen Redis-Replica | `6379` |
| `AG_REDIS_MASTER_HOST` | Hostname/IP des Redis Masters (Write) | `127.0.0.1` |
| `AG_REDIS_MASTER_PORT` | Port des Redis Masters | `6379` |
