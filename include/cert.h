#ifndef CERT_H
#define CERT_H

// Self-signed certificate for HTTPS server (TLS termination)
// This certificate is used by the ESP32-S3 to accept HTTPS connections on the Ethernet side

// External declarations for certificate and key embedded in binary
extern const uint8_t server_cert_pem_start[] asm("_binary_server_cert_pem_start");
extern const uint8_t server_cert_pem_end[]   asm("_binary_server_cert_pem_end");
extern const uint8_t server_key_pem_start[] asm("_binary_server_key_pem_start");
extern const uint8_t server_key_pem_end[]   asm("_binary_server_key_pem_end");

#endif // CERT_H
