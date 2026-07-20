#!/bin/bash
# SUCO Lite Linux Installer
# Muss als root ausgeführt werden!

set -e

# 1. Root-Rechte prüfen
if [ "$EUID" -ne 0 ]; then
    echo "Bitte starte dieses Skript mit sudo oder als root!"
    exit 1
fi

echo -e "\e[36m=============================================\e[0m"
echo -e "\e[36m      SUCO Lite - Linux Installer\e[0m"
echo -e "\e[36m=============================================\e[0m"
echo ""

# 2. Installationsmodus abfragen
echo "Wähle den Installationsmodus:"
echo "1) Coordinator + Worker (Hauptrechner)"
echo "2) Nur Worker (Kompilierungs-Knoten)"
read -p "Auswahl [1-2]: " choice

# 3. Installationsverzeichnisse erstellen
mkdir -p /usr/local/bin
mkdir -p /usr/local/share/suco

# 4. Binaries kopieren
BUILD_DIR="./build_linux"
if [ ! -f "$BUILD_DIR/suco" ]; then
    BUILD_DIR="./build" # Fallback
fi

if [ ! -f "$BUILD_DIR/suco" ]; then
    echo "Fehler: Kompilierte Binärdateien nicht gefunden. Bitte baue das Projekt zuerst!"
    exit 1
fi

cp "$BUILD_DIR/suco" /usr/local/bin/
cp "./dashboard.html" /usr/local/share/suco/

if [ "$choice" == "1" ]; then
    cp "$BUILD_DIR/suco-coordinator" /usr/local/bin/
    cp "$BUILD_DIR/suco-worker" /usr/local/bin/
    echo "Binärdateien für Client, Coordinator und Worker wurden nach /usr/local/bin/ kopiert."
else
    cp "$BUILD_DIR/suco-worker" /usr/local/bin/
    echo "Binärdateien für Worker wurden nach /usr/local/bin/ kopiert."
fi

# 5. Firewall-Regeln erstellen (UFW falls installiert)
if command -v ufw >/dev/null 2>&1; then
    echo "Richte UFW-Firewall-Regeln ein..."
    ufw allow 9000/tcp comment "SUCO Compile TCP"
    ufw allow 9001/tcp comment "SUCO Dashboard HTTP"
    ufw allow 9002/udp comment "SUCO Discovery UDP"
    echo "Firewall-Regeln registriert."
else
    echo "UFW nicht gefunden. Bitte stelle sicher, dass die Ports 9000 (TCP), 9001 (TCP) und 9002 (UDP) geöffnet sind."
fi

# 6. Systemd Services einrichten
if [ "$choice" == "1" ]; then
    # Starte Coordinator Service
    cat <<EOF >/etc/systemd/system/suco-coordinator.service
[Unit]
Description=SUCO Compile Coordinator
After=network.target

[Service]
Type=simple
WorkingDirectory=/usr/local/share/suco
ExecStart=/usr/local/bin/suco-coordinator
Restart=always

[Install]
WantedBy=multi-user.target
EOF

    # Starte Worker Service (mit weniger Kernen, z.B. nproc - 2)
    cores=$(nproc)
    worker_cores=$((cores - 2))
    if [ $worker_cores -lt 1 ]; then
        worker_cores=1
    fi

    cat <<EOF >/etc/systemd/system/suco-worker.service
[Unit]
Description=SUCO Compile Worker
After=network.target

[Service]
Type=simple
WorkingDirectory=/usr/local/share/suco
ExecStart=/usr/local/bin/suco-worker --slots $worker_cores
Restart=always

[Install]
WantedBy=multi-user.target
EOF

    systemctl daemon-reload
    systemctl enable suco-coordinator.service suco-worker.service
    systemctl restart suco-coordinator.service suco-worker.service

    echo -e "\e[32mSystemd-Dienste (suco-coordinator und suco-worker) wurden eingerichtet und gestartet.\e[0m"
    echo -e "\e[32mDas Live-Dashboard ist unter http://localhost:9001 erreichbar.\e[0m"
else
    # Nur Worker Service
    cores=$(nproc)

    cat <<EOF >/etc/systemd/system/suco-worker.service
[Unit]
Description=SUCO Compile Worker
After=network.target

[Service]
Type=simple
WorkingDirectory=/usr/local/share/suco
ExecStart=/usr/local/bin/suco-worker --slots $cores
Restart=always

[Install]
WantedBy=multi-user.target
EOF

    systemctl daemon-reload
    systemctl enable suco-worker.service
    systemctl restart suco-worker.service

    echo -e "\e[32mSystemd-Dienst (suco-worker) wurde eingerichtet und gestartet.\e[0m"
fi

echo ""
echo -e "\e[36m=============================================\e[0m"
echo -e "\e[36m     Installation erfolgreich abgeschlossen!\e[0m"
echo -e "\e[36m=============================================\e[0m"
