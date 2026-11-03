#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>

#define BUFLEN 512

// --- 1. STRUKTURA DŻINA I MATRYCY W RAM ---
typedef struct {
    unsigned char* matrix_buffer; // Wskaźnik na surowy plik matrycy w RAM
    size_t matrix_size;           // Rozmiar w bajtach
} GenieEngine;

// Kody akcji rozpoznawane przez Dżina
#define ACTION_UNKNOWN      0
#define ACTION_FTP_BANNER   1 // 220
#define ACTION_FTP_PASS     2 // 331
#define ACTION_FTP_LOGGED   3 // 230
#define ACTION_FTP_PASV     4 // 227 (Passive Mode)
#define ACTION_FTP_ERROR    5 // 530 / Błędy

// --- 2. WPROWADZENIE MATRYCY .ONNX DO PAMIĘCI (fopen + malloc) ---
int init_genie_matrix(GenieEngine* genie, const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) {
        printf("🧞 [Dżin]: Brak pliku matrycy '%s'. Działam w trybie standardowym.\n", filename);
        genie->matrix_buffer = NULL;
        genie->matrix_size = 0;
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    genie->matrix_buffer = (unsigned char*)malloc(size);
    if (!genie->matrix_buffer) {
        perror("🧞 [Dżin]: Błąd malloc dla matrycy");
        fclose(f);
        return -1;
    }

    fread(genie->matrix_buffer, 1, size, f);
    fclose(f);
    
    genie->matrix_size = (size_t)size;
    printf("🧞 [Dżin]: Pomyślnie wczytano matrycę '%s' do RAM (%ld bajtów surowego bufora)\n", filename, size);
    return 0;
}

// --- 3. SILNIK DECYZYJNY DŻINA ---
int ask_genie(const char* response) {
    if (strstr(response, "220 ") != NULL) return ACTION_FTP_BANNER;
    if (strstr(response, "331 ") != NULL) return ACTION_FTP_PASS;
    if (strstr(response, "230 ") != NULL) return ACTION_FTP_LOGGED;
    if (strstr(response, "227 ") != NULL) return ACTION_FTP_PASV;
    if (strstr(response, "530 ") != NULL) return ACTION_FTP_ERROR;
    return ACTION_UNKNOWN;
}

// --- 4. PARSER TRYBU PASYWNEGO (227) ---
int parse_pasv_response(const char* response, char* ip_out, size_t ip_len, int* port_out) {
    int h1, h2, h3, h4, p1, p2;
    char* ptr = (char*)strchr(response, '(');
//    char* ptr = strchr(response, '(');
    if (!ptr) return -1;

    if (sscanf(ptr, "(%d,%d,%d,%d,%d,%d)", &h1, &h2, &h3, &h4, &p1, &p2) == 6) {
        snprintf(ip_out, ip_len, "%d.%d.%d.%d", h1, h2, h3, h4);
        *port_out = (p1 * 256) + p2;
        return 0;
    }
    return -1;
}

// --- 5. POBIERANIE LISTY KATALOGÓW (LIST) ---
void fetch_ftp_list(int control_sock, const char* data_ip, int data_port) {
    int data_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (data_sock < 0) {
        perror("Błąd tworzenia socketu danych");
        return;
    }

    struct sockaddr_in data_server;
    memset(&data_server, 0, sizeof(data_server));
    data_server.sin_family = AF_INET;
    data_server.sin_addr.s_addr = inet_addr(data_ip);
    data_server.sin_port = htons(data_port);

    if (connect(data_sock, (struct sockaddr*)&data_server, sizeof(data_server)) < 0) {
        perror("Błąd połączenia z socketem danych");
        close(data_sock);
        return;
    }

    printf("🧞 [Dżin - Dane]: Połączono z socketem danych na %s:%d\n", data_ip, data_port);

    const char* cmd = "LIST\r\n";
    send(control_sock, cmd, strlen(cmd), 0);

    unsigned char data_buf[BUFLEN];
    int rv;
    printf("\n--- 📂 LISTA PLIKÓW Z SERWERA ---\n");
    while ((rv = recv(data_sock, data_buf, BUFLEN - 1, 0)) > 0) {
        data_buf[rv] = '\0';
        printf("%s", data_buf);
    }
    printf("--- 🏁 KONIEC LISTY ---\n\n");

    close(data_sock);
}

// --- 6. POBIERANIE PLIKU Z LICZNIKIEM (RETR / get) ---
void fetch_ftp_file(int control_sock, const char* data_ip, int data_port, const char* filename) {
    FILE* local_file = fopen(filename, "wb");
    if (!local_file) {
        perror("🧞 [Błąd]: Nie można utworzyć lokalnego pliku do zapisu");
        return;
    }

    int data_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (data_sock < 0) {
        perror("Błąd tworzenia socketu danych");
        fclose(local_file);
        return;
    }

    struct sockaddr_in data_server;
    memset(&data_server, 0, sizeof(data_server));
    data_server.sin_family = AF_INET;
    data_server.sin_addr.s_addr = inet_addr(data_ip);
    data_server.sin_port = htons(data_port);

    if (connect(data_sock, (struct sockaddr*)&data_server, sizeof(data_server)) < 0) {
        perror("Błąd połączenia z socketem danych");
        close(data_sock);
        fclose(local_file);
        return;
    }

    printf("🧞 [Dżin - Pobieranie]: Połączono z socketem danych dla pliku '%s'\n", filename);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "RETR %s\r\n", filename);
    send(control_sock, cmd, strlen(cmd), 0);

    unsigned char data_buf[BUFLEN];
    int rv;
    long total_bytes = 0;

    printf("⏳ Pobieranie w toku...\n");
    while ((rv = recv(data_sock, data_buf, BUFLEN, 0)) > 0) {
        fwrite(data_buf, 1, rv, local_file);
        total_bytes += rv;
        printf("\r📥 Pobrano: %ld bajtów (%.2f KB)", total_bytes, (float)total_bytes / 1024.0);
        fflush(stdout);
    }

    printf("\n✅ [Sukces]: Pobrano plik '%s' (%ld bajtów).\n\n", filename, total_bytes);

    fclose(local_file);
    close(data_sock);
}

// --- 7. WYSYŁANIE PLIKU Z LICZNIKIEM (STOR / put) ---
void send_ftp_file(int control_sock, const char* data_ip, int data_port, const char* filename) {
    FILE* local_file = fopen(filename, "rb");
    if (!local_file) {
        perror("🧞 [Błąd]: Nie można otworzyć lokalnego pliku do wysłania");
        return;
    }

    int data_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (data_sock < 0) {
        perror("Błąd tworzenia socketu danych");
        fclose(local_file);
        return;
    }

    struct sockaddr_in data_server;
    memset(&data_server, 0, sizeof(data_server));
    data_server.sin_family = AF_INET;
    data_server.sin_addr.s_addr = inet_addr(data_ip);
    data_server.sin_port = htons(data_port);

    if (connect(data_sock, (struct sockaddr*)&data_server, sizeof(data_server)) < 0) {
        perror("Błąd połączenia z socketem danych");
        close(data_sock);
        fclose(local_file);
        return;
    }

    printf("🧞 [Dżin - Wysyłanie]: Połączono z socketem danych dla pliku '%s'\n", filename);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "STOR %s\r\n", filename);
    send(control_sock, cmd, strlen(cmd), 0);

    unsigned char data_buf[BUFLEN];
    size_t bytes_read = 0;
    long total_sent = 0;

    printf("⏳ Wysyłanie w toku...\n");
    while ((bytes_read = fread(data_buf, 1, BUFLEN, local_file)) > 0) {
        int sent = send(data_sock, data_buf, bytes_read, 0);
        if (sent < 0) break;
        total_sent += sent;
        printf("\r📤 Wysłano: %ld bajtów (%.2f KB)", total_sent, (float)total_sent / 1024.0);
        fflush(stdout);
    }

    printf("\n✅ [Sukces]: Wysłano plik '%s' na serwer (%ld bajtów).\n\n", filename, total_sent);

    fclose(local_file);
    close(data_sock);
}

// --- 8. ROZWIĄZYWANIE HOSTA ---
char* resolve_hostname(char* hostname) {
    struct hostent* he;
    if ((he = gethostbyname(hostname)) == NULL) {
        herror("gethostbyname");
        return NULL;
    }
    struct in_addr** addr_list = (struct in_addr**) he->h_addr_list;
    if (addr_list[0] != NULL) {
        return inet_ntoa(*addr_list[0]);
    }
    return NULL;
}

// --- 9. MAIN ---
int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Użycie: %s <adres_serwera> <port>\n", argv[0]);
        return 1;
    }

    char* hostname = argv[1];
    int port = atoi(argv[2]);

    GenieEngine genie;
    init_genie_matrix(&genie, "network_anomaly.onnx");

    char* ip_address = resolve_hostname(hostname);
    if (!ip_address) return 1;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("Socket error");
        if (genie.matrix_buffer) free(genie.matrix_buffer);
        return 1;
    }

    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = inet_addr(ip_address);
    server.sin_port = htons(port);

    if (connect(sock, (struct sockaddr*)&server, sizeof(server)) < 0) {
        perror("Connect error");
        close(sock);
        if (genie.matrix_buffer) free(genie.matrix_buffer);
        return 1;
    }

    printf("Połączono z serwerem FTP %s:%d\n", ip_address, port);

    // Stan automatyzacji PASV
    char active_pasv_ip[64] = {0};
    int active_pasv_port = 0;
    int pending_action = 0;           // 0: brak, 1: LIST, 2: GET, 3: PUT
    char pending_filename[256] = {0};

    unsigned char buf[BUFLEN];
    struct timeval ts = {1, 0};

    while (1) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        FD_SET(STDIN_FILENO, &fds);

        int nready = select(sock + 1, &fds, NULL, NULL, &ts);
        if (nready < 0) break;
        if (nready == 0) {
            ts.tv_sec = 1;
            continue;
        }

        // 1. Odbiór z serwera FTP
        if (FD_ISSET(sock, &fds)) {
            int rv = recv(sock, buf, BUFLEN - 1, 0);
            if (rv <= 0) {
                printf("Rozłączono z serwerem.\n");
                break;
            }
            buf[rv] = '\0';
            printf("%s", buf);

            int action = ask_genie((char*)buf);

            switch (action) {
                case ACTION_FTP_BANNER:
                    printf("🧞 [Dżin]: Wykryto powitanie serwera.\n");
                    break;
                case ACTION_FTP_PASS:
                    printf("🧞 [Dżin]: Serwer oczekuje na hasło.\n");
                    break;
                case ACTION_FTP_LOGGED:
                    printf("🧞 [Dżin]: Zalogowano pomyślnie do systemu!\n");
                    break;
                case ACTION_FTP_PASV:
                    if (parse_pasv_response((char*)buf, active_pasv_ip, sizeof(active_pasv_ip), &active_pasv_port) == 0) {
                        printf("🧞 [Dżin - PASV]: Otwarto kanał danych na %s:%d\n", active_pasv_ip, active_pasv_port);

                        if (pending_action == 1) {
                            fetch_ftp_list(sock, active_pasv_ip, active_pasv_port);
                            pending_action = 0;
                        } else if (pending_action == 2) {
                            fetch_ftp_file(sock, active_pasv_ip, active_pasv_port, pending_filename);
                            pending_action = 0;
                        } else if (pending_action == 3) {
                            send_ftp_file(sock, active_pasv_ip, active_pasv_port, pending_filename);
                            pending_action = 0;
                        }
                    }
                    break;
                case ACTION_FTP_ERROR:
                    printf("⚠️ [Dżin]: Wykryto błąd serwera (530)!\n");
                    break;
                default:
                    break;
            }
        }

        // 2. Wysyłanie z klawiatury
        if (FD_ISSET(STDIN_FILENO, &fds)) {
            char input_buf[256];
            if (fgets(input_buf, sizeof(input_buf), stdin) != NULL) {
                size_t len = strlen(input_buf);
                if (len > 0 && input_buf[len-1] == '\n') {
                    input_buf[len-1] = '\0';
                }

                // Komenda LIST (automatyczny PASV)
                if (strcasecmp(input_buf, "list") == 0) {
                    pending_action = 1;
                    const char* pasv_cmd = "PASV\r\n";
                    send(sock, pasv_cmd, strlen(pasv_cmd), 0);
                    continue;
                }

                // Komenda GET (automatyczny PASV + RETR)
                if (strncasecmp(input_buf, "get ", 4) == 0) {
                    pending_action = 2;
                    snprintf(pending_filename, sizeof(pending_filename), "%s", input_buf + 4);
                    const char* pasv_cmd = "PASV\r\n";
                    send(sock, pasv_cmd, strlen(pasv_cmd), 0);
                    continue;
                }

                // Komenda PUT (automatyczny PASV + STOR)
                if (strncasecmp(input_buf, "put ", 4) == 0) {
                    pending_action = 3;
                    snprintf(pending_filename, sizeof(pending_filename), "%s", input_buf + 4);
                    const char* pasv_cmd = "PASV\r\n";
                    send(sock, pasv_cmd, strlen(pasv_cmd), 0);
                    continue;
                }

                // NOWOŚĆ: Tworzenie katalogu (MKD) - bez trybu pasywnego
                if (strncasecmp(input_buf, "mkdir ", 6) == 0) {
                    char mkd_cmd[512];
                    snprintf(mkd_cmd, sizeof(mkd_cmd), "MKD %s\r\n", input_buf + 6);
                    send(sock, mkd_cmd, strlen(mkd_cmd), 0);
                    continue;
                }

                // Standardowe wysłanie innych komend (user, pass, pwd, help itp.)
                strcat(input_buf, "\r\n");
                send(sock, input_buf, strlen(input_buf), 0);
            }
        }
    }

    close(sock);
    if (genie.matrix_buffer) {
        free(genie.matrix_buffer);
        printf("🧞 [Dżin]: Zwolniono matrycę z RAM.\n");
    }

    return 0;
}