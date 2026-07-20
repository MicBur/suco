#pragma once

#include "socket_util.h"
#include <string>
#include <mutex>
#include <stdint.h>

namespace suco::worker {

class NetworkClient {
public:
    NetworkClient();
    ~NetworkClient();

    // Verhindere Kopieren
    NetworkClient(const NetworkClient&) = delete;
    NetworkClient& operator=(const NetworkClient&) = delete;

    /**
     * @brief Sucht den Coordinator im Netzwerk über UDP-Broadcast.
     * @param out_ip Die gefundene IP-Adresse des Coordinators.
     * @param out_port Der gefundene Port des Coordinators.
     * @return true wenn erfolgreich, false sonst.
     */
    bool discover_coordinator(std::string& out_ip, uint16_t& out_port);

    /**
     * @brief Verbindet den TCP-Socket mit dem Coordinator.
     * @param host Die IP-Adresse oder der Hostname des Coordinators.
     * @param port Der TCP-Port des Coordinators.
     * @return true wenn die Verbindung erfolgreich hergestellt wurde.
     */
    bool connect_to(const std::string& host, uint16_t port);

    /**
     * @brief Schließt die aktive TCP-Verbindung.
     */
    void disconnect();

    /**
     * @brief Registriert den Worker beim Coordinator.
     *        Sendet das initiale PACKET_HEARTBEAT-Paket mit Slots, Hostname und OS.
     * @param name Der Name des Workers (Hostname).
     * @param os Das Betriebssystem des Workers.
     * @param slots Die Anzahl der maximal verfügbaren Kompilier-Slots.
     * @return true wenn die Registrierung erfolgreich versendet wurde.
     */
    bool register_worker(const std::string& name, const std::string& os, int slots, const std::string& toolchains_json, uint16_t direct_port);

    /**
     * @brief Sendet Daten thread-sicher über den Socket.
     * @param data Zeiger auf die zu sendenden Daten.
     * @param len Länge der Daten in Bytes.
     * @return true wenn das Senden erfolgreich war.
     */
    bool send_packet(const void* data, size_t len);

    /**
     * @brief Liest Daten blockierend aus dem Socket.
     * @param data Zeiger auf den Puffer, in den gelesen wird.
     * @param len Anzahl der zu lesenden Bytes.
     * @return true wenn alle Bytes erfolgreich gelesen wurden.
     */
    bool receive_packet(void* data, size_t len);

    /**
     * @brief Prüft, ob der Client aktuell verbunden ist.
     */
    bool is_connected() const { return m_sock != INVALID_SOCKET_VAL; }

    /**
     * @brief Gibt den rohen Socket-Deskriptor zurück.
     */
    socket_t get_socket() const { return m_sock; }

private:
    socket_t m_sock = INVALID_SOCKET_VAL;
    std::mutex m_send_mutex;
    std::mutex m_receive_mutex;
};

} // namespace suco::worker
