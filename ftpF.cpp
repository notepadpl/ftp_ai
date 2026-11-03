#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten/emscripten.h>
#include <emscripten/fetch.h>

// --- DYNAMICZNY STAN SESJI FTP ---
static char current_host[128] = "";
static int current_port = 21;
static char current_user[64] = "anonymous";
static char current_pass[64] = "anonymous@";
static char current_path[256] = "/";

static char g_async_response[8192] = "🧞 [Dżin]: Gotowy. Połącz się z serwerem za pomocą komendy 'open <host> [port]'.";

// Adres bramki HTTP-to-FTP (może być modyfikowany)
static char gateway_url[256] = "https://twoja-bramka-ftp.herokuapp.com/api"; 

// --- CALLBACKI DLA EMSCRIPTEN_FETCH ---
void ftp_success_callback(emscripten_fetch_t *fetch) {
    size_t bytes_to_copy = fetch->numBytes < sizeof(g_async_response) - 1 
                           ? fetch->numBytes 
                           : sizeof(g_async_response) - 1;
                           
    memcpy(g_async_response, fetch->data, bytes_to_copy);
    g_async_response[bytes_to_copy] = '\0';

    emscripten_fetch_close(fetch);
}

void ftp_error_callback(emscripten_fetch_t *fetch) {
    snprintf(g_async_response, sizeof(g_async_response), 
             "❌ Błąd połączenia z bramką dla serwera %s:%d (Kod HTTP: %d).", 
             current_host, current_port, fetch->status);
    emscripten_fetch_close(fetch);
}

// Funkcja budująca uniwersalne żądanie do bramki
void send_universal_ftp_request(const char* action, const char* extra_param) {
    if (strlen(current_host) == 0) {
        snprintf(g_async_response, sizeof(g_async_response), "⚠️ Błąd: Nie ustawiono serwera FTP! Wpisz 'open <adres_serwera>'.");
        return;
    }

    char full_url[1024];
    snprintf(full_url, sizeof(full_url), 
             "%s?host=%s&port=%d&user=%s&pass=%s&action=%s&path=%s&param=%s", 
             gateway_url, current_host, current_port, current_user, current_pass, 
             action, current_path, extra_param ? extra_param : "");

    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    strcpy(attr.requestMethod, "GET");
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY; // Poprawiona nazwa flagi
    attr.onsuccess = ftp_success_callback;
    attr.onerror = ftp_error_callback;

    emscripten_fetch(&attr, full_url); // Zmieniona kolejność: najpierw &attr, potem URL
}

// --- EKSPORTOWANA FUNKCJA OBSŁUGI KOMEND ---
extern "C" {

EMSCRIPTEN_KEEPALIVE
const char* execute_shell_command(const char* cmd) {
    static char local_response[2048];

    // 1. Łączenie z dowolnym serwerem: "open ftp.dlptest.com 21" lub "open ftp.gnu.org"
    if (strncmp(cmd, "open ", 5) == 0 || strncmp(cmd, "connect ", 8) == 0) {
        const char* args = strchr(cmd, ' ') + 1;
        int parsed_port = 21;
        char parsed_host[128];

        if (sscanf(args, "%127s %d", parsed_host, &parsed_port) >= 1) {
            strncpy(current_host, parsed_host, sizeof(current_host) - 1);
            current_port = parsed_port;
            strcpy(current_path, "/"); // Resetuj ścieżkę przy nowym połączeniu
            
            snprintf(local_response, sizeof(local_response), 
                     "🔗 Ustawiono docelowy serwer: %s:%d\nUżyj 'login <user> <pass>' lub domyślnie (anonymous).", 
                     current_host, current_port);
        } else {
            snprintf(local_response, sizeof(local_response), "❌ Składnia: open <adres_hosta> [port]");
        }
        return local_response;
    }

    // 2. Logowanie: "login użytkownik hasło"
    if (strncmp(cmd, "login ", 6) == 0) {
        char user[64], pass[64];
        if (sscanf(cmd + 6, "%63s %63s", user, pass) == 2) {
            strncpy(current_user, user, sizeof(current_user) - 1);
            strncpy(current_pass, pass, sizeof(current_pass) - 1);
            snprintf(local_response, sizeof(local_response), "🔑 Zapisano poświadczenia dla użytkownika: %s", current_user);
        } else {
            snprintf(local_response, sizeof(local_response), "❌ Składnia: login <użytkownik> <hasło>");
        }
        return local_response;
    }

    // 3. Zmiana katalogu zdalnego: "cd /pub"
    if (strncmp(cmd, "cd ", 3) == 0) {
        strncpy(current_path, cmd + 3, sizeof(current_path) - 1);
        snprintf(local_response, sizeof(local_response), "📁 Zmieniono lokalną ścieżkę na: %s", current_path);
        return local_response;
    }

    // 4. Pobranie listy plików z aktualnie wybranego serwera: "list"
    if (strcmp(cmd, "list") == 0 || strcmp(cmd, "ls") == 0) {
        send_universal_ftp_request("list", "");
        snprintf(local_response, sizeof(local_response), 
                 "⏳ Pobieram listę plików z %s:%d%s...\nWpisz 'status', aby zobaczyć wynik.", 
                 current_host, current_port, current_path);
        return local_response;
    }

    // 5. Pobranie konkretnego pliku: "get nazwa_pliku.txt"
    if (strncmp(cmd, "get ", 4) == 0) {
        const char* file_name = cmd + 4;
        send_universal_ftp_request("download", file_name);
        snprintf(local_response, sizeof(local_response), "⏳ Zlecono pobieranie pliku '%s'...\nWpisz 'status', aby odczytać zawartość.", file_name);
        return local_response;
    }

    // 6. Odczyt stanu sesji i informacji
    if (strcmp(cmd, "pwd") == 0) {
        snprintf(local_response, sizeof(local_response), 
                 "📊 [Stan Sesji]\n - Host: %s:%d\n - User: %s\n - Ścieżka: %s", 
                 strlen(current_host) > 0 ? current_host : "(brak)", 
                 current_port, current_user, current_path);
        return local_response;
    }

    // 7. Wynik operacji asynchronicznej
    if (strcmp(cmd, "status") == 0 || strcmp(cmd, "result") == 0) {
        return g_async_response;
    }

    // 8. Pomoc
    if (strcmp(cmd, "help") == 0) {
        return "📜 [Uniwersalny Dżin FTP] Komendy:\n"
               " - open <host> [port]  : Ustaw dowolny serwer FTP\n"
               " - login <user> <pass> : Ustaw dane logowania\n"
               " - cd <katalog>        : Zmień ścieżkę\n"
               " - list                : Pobierz listę plików\n"
               " - get <plik>          : Pobierz zawartość pliku\n"
               " - pwd                 : Pokaż stan aktualnego połączenia\n"
               " - status              : Wyświetl odpowiedź z sieci";
    }

    snprintf(local_response, sizeof(local_response), "🧞 [Dżin]: Nieznana komenda '%s'. Wpisz 'help'.", cmd);
    return local_response;
}

} // Koniec extern "C"

int main(int argc, char** argv) {
    printf("🧞 [C++ Main]: Uniwersalny silnik FTP zainicjalizowany.\n");
    return 0;
}