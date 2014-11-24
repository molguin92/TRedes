#define DID   0
#define DTYPE 1
#define DSEQ 2
#define DRTN 3 /* Nro Retrans */
#define DHDR  4

#define MAX_SQN 255 /* nros de secuencia disponibles (0 - 255) */
#define WND_SIZE 50 /* tamano ventana */

#define BUF_SIZE 1400+DHDR

#define DATA 'D'
#define ACK  'A'
#define CONNECT 'C'
#define CLOSE 'E'

#define CONNECTED 1
#define FREE 2
#define CLOSED 3

#define TIMEOUT 3.0
#define INTTIMEOUT 3
#define RETRIES 10

#define NO_INTERR 0
#define INTERR_RSEND 1
#define INTERR_DATA 2

extern int Data_debug;

int Dconnect(char *hostname, char *port);
void Dbind(void* (*f)(void *), char *port);

int Dread(int cl, char *buf, int l);
void Dwrite(int cl, char *buf, int l);
void Dclose(int cl);
