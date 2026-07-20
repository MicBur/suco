#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <set>
#include <condition_variable>

namespace suco::worker {

/**
 * @brief Thread-sicherer Cache für Precompiled Headers (PCH).
 */
class HeaderCache {
public:
    /**
     * @brief Liefert die Singleton-Instanz des HeaderCaches.
     */
    static HeaderCache& get_instance();

    /**
     * @brief Initialisiert den Cache mit Verzeichnis und maximaler Größe.
     */
    void initialize(const std::string& cache_dir, int max_size_gb);

    /**
     * @brief Prüft, ob ein PCH für einen bestimmten Hash vorliegt.
     */
    bool has(const std::string& hash);

    /**
     * @brief Holt den Pfad zum PCH-Verzeichnis für den übergebenen Hash.
     * @param hash Der header_set_hash.
     * @param pch_path_out Ausgabepfad zur PCH (.gch Datei).
     * @return true, falls der Cache-Eintrag existiert, false sonst.
     */
    bool get(const std::string& hash, std::string& pch_path_out);

    /**
     * @brief Liefert den gecachten (bereits gefilterten) Header-Set-Quelltext.
     * @param hash Der header_set_hash.
     * @param source_out Ausgabe: Inhalt der `header`-Datei des Cache-Eintrags.
     * @return true, falls der Quelltext gelesen werden konnte, sonst false.
     *
     * Wird für den PCH-Fehler-Fallback benötigt, wenn der Client den
     * header_set_source weggelassen hat (Optimierung „PCH bereits bekannt"):
     * der Worker rekonstruiert die vollständige TU aus seinem eigenen Cache.
     */
    bool get_source(const std::string& hash, std::string& source_out);

    /**
     * @brief Counts how often this header set was requested on THIS worker and returns
     *        the new count. Used to decide whether building a PCH is worth it — a PCH
     *        costs ~2.3s to build and saves only ~0.6s per TU (measured on RocksDB), so
     *        it only pays off once the same set is reused several times on the same node.
     */
    int note_use(const std::string& hash);

    /**
     * @brief Writes the header set's TEXT (no PCH compile). Cheap and always worth it:
     *        GCC's -include resolves to header.gch when one sits next to the file and
     *        falls back to reading the text otherwise, so a stored source alone already
     *        makes the job compilable — the .gch is a pure speed-up added later.
     */
    bool store_source(const std::string& hash, const std::string& preprocessed_header,
                      const std::string& orig_cmd);

    /** @brief True if a compiled PCH (header.gch) exists for this hash. */
    bool has_pch(const std::string& hash);

    /**
     * @brief Kompiliert das Header-Set und speichert das Ergebnis als PCH (.gch).
     * @param hash Der header_set_hash.
     * @param preprocessed_header Der Inhalt des Header-Sets (preprocessed C++).
     * @param orig_cmd Der originale Kompilierbefehl.
     * @param toolchain_hash Der toolchain_hash (falls vorhanden).
     * @return true bei Erfolg, false bei Fehlern.
     */
    bool store(const std::string& hash,
               const std::string& preprocessed_header,
               const std::string& orig_cmd,
               const std::string& toolchain_hash);

    /**
     * @brief Bereinigt den Cache per LRU, falls das Speicherlimit überschritten wird.
     */
    void cleanup_lru();

    /**
     * @brief Löscht alle Einträge im Header-Cache (PCHs).
     */
    void clear();

   private:
    HeaderCache() = default;
    ~HeaderCache() = default;
    HeaderCache(const HeaderCache&) = delete;
    HeaderCache& operator=(const HeaderCache&) = delete;

    std::string cache_dir_;
    size_t max_size_bytes_ = 8ULL * 1024 * 1024 * 1024; // Default: 8 GB
    std::mutex mutex_;
    // How often each header set was requested on this worker (see note_use).
    std::unordered_map<std::string, int> use_counts_;
    std::set<std::string> active_compilations_;
    std::condition_variable cv_;
};

} // namespace suco::worker
