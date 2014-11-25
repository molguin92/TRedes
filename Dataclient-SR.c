#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <string.h>
#include "jsocket6.4.h"
#include "bufbox.h"
#include "Data-SR.h"

#define MAX_QUEUE 100 /* buffers en boxes */

/* Version Selective Repeat */

static void updRTT( unsigned char seqn ); /* funcion para actualizar RTT y timeout */
static void sendPacket( int seqn ); /* reenvia un paquete */
static int in_SWindow ( unsigned char seqn ); /* verifica que seqn este en la ventana de envio */
static int in_RWindow ( unsigned char seqn ); /* verifica que seqn este en la ventana de recepcion */
static int checkSpaceInWindow();

int Data_debug = 0; /* para debugging */

/* Variables globales del cliente */
static int Dsock = -1;
static pthread_mutex_t Dlock;
static pthread_cond_t  Dcond;
static pthread_t Drcvr_pid, Dsender_pid;
static unsigned char ack[DHDR] = {0, ACK, 0, 0};

static void *Dsender ( void *ppp );
static void *Drcvr ( void *ppp );


#define max(a, b) (((a) > (b))?(a):(b))

struct
{
    BUFBOX *rbox,
           *wbox;                    /* Cajas de comunicación con el thread "usuario" */
    int pending_sz[MAX_SQN + 1];    /* tamano de los buffers */
    int retries[MAX_SQN + 1];       /* retries por paquete */
    unsigned char swindow[MAX_SQN + 1][BUF_SIZE]; /* ventana de envio */
    unsigned char rwindow[MAX_SQN + 1][BUF_SIZE]; /* ventana de recepcion */
    unsigned char exp_ack[MAX_SQN + 1];  /* ACKs esperando */
    unsigned char exp_dat[MAX_SQN + 1];  /* data esperando */
    unsigned char resend[MAX_SQN + 1];   /* flags para indicar reenvio */
    double timeout[MAX_SQN + 1];    /* tiempo restante antes de retransmision */
    double timestamp[MAX_SQN + 1];  /* marca cuando fue enviado el ultimo paquete */
    int eof;
    double rtt;         /* RTT */
    double mdev;
    int state;          /* FREE, CONNECTED, CLOSED */
    int id;             /* id conexión, la asigna el servidor */
    int lar;            /* last ack received */
    int lfs;            /* last frame sent */
    int laf;            /* largest acceptable frame */
    int lfr;            /* last frame received */

    int nack_cnt;        /* nro de acks incorrectos */
} connection;

/* Funciones utilitarias */

/* retorna hora actual */
double Now()
{
    struct timespec tt;
#ifdef __MACH__ // OS X does not have clock_gettime, use clock_get_time
    clock_serv_t cclock;
    mach_timespec_t mts;
    host_get_clock_service ( mach_host_self(), CALENDAR_CLOCK, &cclock );
    clock_get_time ( cclock, &mts );
    mach_port_deallocate ( mach_task_self(), cclock );
    tt.tv_sec = mts.tv_sec;
    tt.tv_nsec = mts.tv_nsec;
#else
    clock_gettime ( CLOCK_REALTIME, &tt );
#endif

    return ( tt.tv_sec + 1e-9 * tt.tv_nsec );
}

/* Inicializa estructura conexión y ventana */
int init_connection ( int id )
{
    int i;
    pthread_t pid;
    int *p;

    connection.lar = -1;
    connection.lfs = -1;
    connection.lfr = 0;

    for ( i = 0; i < MAX_SQN; i++ )
    {
        connection.timeout[i] = -1;
        connection.resend[i] = 0;
    }

    connection.laf = connection.lfr + WND_SIZE;

    for ( i = 0; i < WND_SIZE; i++ )
        connection.exp_dat[i] = 1;

    connection.state = CONNECTED;
    connection.wbox = create_bufbox ( MAX_QUEUE );
    connection.rbox = create_bufbox ( MAX_QUEUE );
    connection.eof = 0;
    connection.id = id;
    connection.rtt = TIMEOUT;
    connection.mdev = -1;
    connection.nack_cnt = 0;
    return id;
}

/* borra estructura conexión */
void del_connection()
{
    delete_bufbox ( connection.wbox );
    delete_bufbox ( connection.rbox );
    connection.state = FREE;
}

/* Función que inicializa los threads necesarios: sender y rcvr */
static void Init_Dlayer ( int s )
{

    Dsock = s;

    if ( pthread_mutex_init ( &Dlock, NULL ) != 0 ) fprintf ( stderr,
                "mutex NO\n" );

    if ( pthread_cond_init ( &Dcond, NULL ) != 0 )  fprintf ( stderr, "cond NO\n" );

    pthread_create ( &Dsender_pid, NULL, Dsender, NULL );
    pthread_create ( &Drcvr_pid, NULL, Drcvr, NULL );
}

/* timer para el timeout */
void tick()
{
    return;
}

/* Función que me conecta al servidor e inicializa el mundo */
int Dconnect ( char *server, char *port )
{
    int s, cl, i;
    double ti, tf, rtt;
    struct sigaction new, old;
    unsigned char inbuf[DHDR], outbuf[DHDR];

    if ( Dsock != -1 ) return -1;

    s = j_socket_udp_connect ( server,
                               port ); /* deja "conectado" el socket UDP, puedo usar recv y send */

    if ( s < 0 ) return s;

    /* inicializar conexion */
    bzero ( &new, sizeof new );
    new.sa_flags = 0;
    new.sa_handler = tick;
    sigaction ( SIGALRM, &new, &old );

    outbuf[DTYPE] = CONNECT;
    outbuf[DID] = 0;
    outbuf[DSEQ] = 0;
    outbuf[DRTN] = 0;

    for ( i = 0; i < RETRIES; i++ )
    {
        send ( s, outbuf, DHDR, 0 );
        alarm ( INTTIMEOUT );

        if ( recv ( s, inbuf, DHDR, 0 ) != DHDR ) continue;

        if ( Data_debug ) fprintf ( stderr, "recibo: %c, %d\n", inbuf[DTYPE],
                                        inbuf[DID] );

        alarm ( 0 );

        if ( inbuf[DTYPE] != ACK || inbuf[DSEQ] != 0 ) continue;

        cl = inbuf[DID];
        break;
    }

    sigaction ( SIGALRM, &old, NULL );

    if ( i == RETRIES )
    {
        fprintf ( stderr, "no pude conectarme\n" );
        return -1;
    }

    fprintf ( stderr, "conectado con id=%d\n", cl );
    init_connection ( cl );
    Init_Dlayer ( s ); /* Inicializa y crea threads */
    return cl;
}

/* Lectura */
int Dread ( int cl, char *buf, int l )
{
    int cnt;

    if ( connection.id != cl ) return -1;

    cnt = getbox ( connection.rbox, buf, l );
    return cnt;
}

/* escritura */
void Dwrite ( int cl, char *buf, int l )
{
    if ( connection.id != cl || connection.state != CONNECTED ) return;

    putbox ( connection.wbox, buf, l );
    /* el lock parece innecesario, pero se necesita:
     * nos asegura que Dsender está esperando el lock o el wait
     * y no va a perder el signal!
     */
    pthread_mutex_lock ( &Dlock );
    pthread_cond_signal (
        &Dcond ); /* Le aviso a sender que puse datos para él en wbox */
    pthread_mutex_unlock ( &Dlock );
}

/* cierra conexión */
void Dclose ( int cl )
{
    if ( connection.id != cl ) return;

    close_bufbox ( connection.wbox );
    close_bufbox ( connection.rbox );
}

/*
 * Aquí está toda la inteligencia del sistema:
 * 2 threads: receptor desde el socket y enviador al socket
 */

/* lector del socket: todos los paquetes entrantes */
static void *Drcvr ( void *ppp )
{
    int cnt;
    int cl, i; /* i es un counter de iteracion para copia de strings */
    unsigned char inbuf[BUF_SIZE];
    int found;

    /* Recibo paquete desde el socket */
    while ( ( cnt = recv ( Dsock, inbuf, BUF_SIZE, 0 ) ) > 0 )
    {
        if ( Data_debug )
            fprintf ( stderr, "recv: id=%d, type=%c, seq=%d\n", inbuf[DID], inbuf[DTYPE],
                      inbuf[DSEQ] );

        if ( cnt < DHDR ) continue;

        cl = inbuf[DID];

        if ( cl != connection.id ) continue;

        fprintf ( stderr, "Lock plox\n");
        pthread_mutex_lock ( &Dlock );
        fprintf ( stderr, "Lock plas\n");

        if ( inbuf[DTYPE] == CLOSE )
        {
            if ( Data_debug )
                fprintf ( stderr, "recibo cierre conexión %d, envío ACK\n", cl );

            ack[DID] = cl;
            ack[DTYPE] = ACK;
            ack[DSEQ] = inbuf[DSEQ];
            ack[DRTN] = inbuf[DRTN];

            if ( send ( Dsock, ack, DHDR, 0 ) < 0 )
            {
                perror ( "send" );
                exit ( 1 );
            }

            if ( !in_RWindow(inbuf[DSEQ]) )
            {
                pthread_mutex_unlock ( &Dlock );
                continue;
            }

            connection.exp_dat[inbuf[DSEQ]] = 0;
            connection.state = CLOSED;
            pthread_cond_signal ( &Dcond );
            pthread_mutex_unlock ( &Dlock );
            Dclose ( cl );
        }

        else if ( inbuf[DTYPE] == ACK && connection.state != FREE )
        {
            /* verificamos que este dentro de la ventana y que coincidan
             * los numeros de reenvios*/

             /* RESETEAR VALORES DE RESEND Y TIMEOUTS: LISTO?*/
             /* MOVER VENTANA : LISTO? */

            if ( in_SWindow( inbuf[DSEQ] ) )
            {
                /* recibimos ack y ajustamos ventana */

                if ( Data_debug )
                    fprintf ( stderr, "recv ACK id=%d, SEQ=%d,  LAR=%d, LFS=%d\n",
                            cl, inbuf[DSEQ], connection.lar, connection.lfs );

                /* si DSEQ no corresponde al pkg mas viejo, sumamos al contador
                * de reenvio */
                if ( inbuf[DSEQ] != ( connection.lar + 1) % ( MAX_SQN + 1 ) )
                {
                    connection.nack_cnt++;
                    if ( connection.nack_cnt >= 3 )
                    {
                        connection.resend[( connection.lar + 1) % ( MAX_SQN + 1 )] = 1;
                        connection.nack_cnt = 0;
                    }
                }

                /* movemos la ventana*/
                else /* inbuf[DSEQ] == connection.lar + 1  */
                {
                    connection.lar = ( connection.lar + 1) % ( MAX_SQN + 1 );
                    if ( Data_debug )
                        fprintf ( stderr, "Correr ventana. lar = %d\n", connection.lar );
                }

                /* reseteamos valores de resend y timeout */
                connection.resend[inbuf[DSEQ]] = 0;
                connection.timeout[inbuf[DSEQ]]  = -1;
                connection.exp_ack[inbuf[DSEQ]] = 0;

                /* verificamos que el nro de retrans coincida y
                 * actualizamos el RTT */
                if ( connection.swindow[inbuf[DSEQ]][DRTN] == inbuf[DRTN] )
                    updRTT ( inbuf[DSEQ] ); /* aqui actualizamos el RTT */

                if ( connection.state == CLOSED )
                {
                    /* conexion cerrada y sin buffers pendientes */
                    del_connection();
                }

                pthread_cond_signal ( &Dcond );

            }

            else /* no esta en ventana */
            {
                connection.resend[inbuf[DSEQ]] = 0;
                connection.timeout[inbuf[DSEQ]]  = -1;
                connection.exp_ack[inbuf[DSEQ]] = 0;

                connection.nack_cnt++;
                if ( connection.nack_cnt >= 3 )
                {
                    connection.resend[( connection.lar + 1) % ( MAX_SQN + 1 )] = 1;
                    connection.nack_cnt = 0;
                }

                pthread_cond_signal ( &Dcond );
            }

        }

        else if ( inbuf[DTYPE] == DATA && connection.state == CONNECTED )
        {
            if ( Data_debug )
                fprintf ( stderr, "rcv: DATA: %d, seq=%d, LFR=%d, LAF=%d\n",
                                            inbuf[DID], inbuf[DSEQ],
                                            connection.lfr, connection.laf );

            if ( boxsz ( connection.rbox ) >= MAX_QUEUE ) /* No tengo espacio */
            {
                if ( Data_debug )
                    fprintf ( stderr, "No hay espacio, descarto\n" );
                pthread_mutex_unlock ( &Dlock );
                continue;
            }

            if ( in_RWindow ( inbuf[DSEQ]) ) /* esta en la ventana */
            {

                ack[DID] = cl;
                ack[DTYPE] = ACK;
                ack[DSEQ] = inbuf[DSEQ];
                ack[DRTN] = inbuf[DRTN];

                /* verificar que no haya sido recibido */

                if ( Data_debug )
                    fprintf ( stderr, "Dentro de Ventana, enviando ACK %d, seq=%d\n", ack[DID], ack[DSEQ] );

                if ( send ( Dsock, ack, DHDR, 0 ) < 0 )
                    perror ( "sendack" );

                if ( !connection.exp_dat[inbuf[DSEQ]] )
                {
                    if ( Data_debug ) fprintf ( stderr, "Duplicado\n" );
                }
                else
                {

                    connection.exp_dat[inbuf[DSEQ]] = 0;

                    /* copiamos a la ventana de recepcion */
                    for ( i = DHDR; i < cnt; i++ )
                        connection.rwindow[inbuf[DSEQ]][i] = inbuf[i];

                    /* si el pkg corresponde al que sigue al ultimo pkg recibido en
                    * orden, corremos la ventana hasta encontrar un pkg todavia no
                    * recibido en orden, y pasamos todos los pkgs en orden al box */
                    if ( inbuf[DSEQ] == ( connection.lfr + 1 ) % ( MAX_SQN + 1 ) )
                    {
                        while ( !connection.exp_dat[( connection.lfr + 1 ) % ( MAX_SQN + 1 )] )
                        {
                            connection.lfr = ( connection.lfr + 1 ) % ( MAX_SQN + 1 );
                            connection.laf = ( connection.laf + 1 ) % ( MAX_SQN + 1 );

                            connection.exp_dat[connection.lfr] = 0;
                            connection.exp_dat[connection.laf] = 1;

                            putbox ( connection.rbox,
                                connection.rwindow[connection.lfr],
                                cnt - DHDR );

                                /* aqui agregar busy-waiting con condition en caso de
                            * que el box de recepcion se llene */

                            while ( boxsz ( connection.rbox ) >= MAX_QUEUE );
                                /* esperar condicion y luego seguir */
                        }
                    }
                }

                pthread_cond_signal ( &Dcond );
            }
            else /* no esta en ventana */
            {

                ack[DID] = cl;
                ack[DTYPE] = ACK;
                ack[DSEQ] = inbuf[DSEQ];
                ack[DRTN] = inbuf[DRTN];

                if ( Data_debug )
                    fprintf ( stderr, "Fuera de Ventana, enviando ACK %d, seq=%d\n", ack[DID],
                                                ack[DSEQ] );

                if ( send ( Dsock, ack, DHDR, 0 ) < 0 )
                    perror ( "sendack" );

                pthread_cond_signal ( &Dcond );

            }

        }

        else if ( Data_debug )
            fprintf ( stderr, "descarto paquete entrante: t=%c, id=%d\n", inbuf[DTYPE],
                      inbuf[DID] );

        pthread_mutex_unlock ( &Dlock );
    }

    fprintf ( stderr, "fallo read en Drcvr()\n" );
    return NULL;
}

int Dclient_timeout_or_pending_data( double *timeout )
/* modificado! ahora devuelve un int que indica el tipo de interrupcion */
/* y guarda el timeout en el puntero entregado */
{

    int i;

    *timeout = Now() + 20;

    if ( connection.state == FREE || connection.eof )
    {
        return NO_INTERR;
    }

    if ( boxsz ( connection.wbox ) != 0
        && checkSpaceInWindow() ) /* verificar que ventana sea menor que wnd_size, arreglar esto */
        {
            /* data from client, and space in the window */
            *timeout = Now();
            return INTERR_DATA;
        }

    for ( i = 0; i < MAX_SQN + 1; i++)
    {
        /* revisamos si hay que reenviar */
        /* revisamos si hay timeouts vencidos */
        if ( connection.resend[i] || ( connection.timeout[i] <= Now() && connection.timeout[i] >= 0 ) )
        {

            fprintf ( stderr, "Resend = %d\n", i );
            connection.resend[i] = 1;
            *timeout = Now();
            return INTERR_RSEND;
        }

        /* si no esta vencido, vemos si por lo menos es menor que
        * el timeout actual */
        if ( connection.timeout[i] <= *timeout && connection.timeout[i] >= 0 )
            *timeout = connection.timeout[i];

    }

    return NO_INTERR;
}

static int checkSpaceInWindow()
{
    if (connection.lfs < connection.lar)
        return ((MAX_SQN - connection.lar) + connection.lfs + 1) <  WND_SIZE;
    else
        return (connection.lfs - connection.lar) < WND_SIZE;
}


/* Thread enviador y retransmisor */
static void *Dsender ( void *ppp )
{
    double timeout;
    struct timespec tt;
    int rc, i, ret;


    for ( ;; )
    {
        pthread_mutex_lock ( &Dlock );

        /* Esperar que pase algo */
        while ( ( rc = Dclient_timeout_or_pending_data ( &timeout ) ) == NO_INTERR );
        {
            // fprintf(stderr, "timeout=%f, now=%f\n", timeout, Now());
            // fprintf(stderr, "Al tuto %f segundos\n", timeout-Now());
            tt.tv_sec = timeout;
            // fprintf(stderr, "Al tuto %f nanos\n", (timeout-tt.tv_sec*1.0));
            tt.tv_nsec = ( timeout - tt.tv_sec * 1.0 ) * 1000000000;
            // fprintf(stderr, "Al tuto %f segundos, %d secs, %d nanos\n", timeout-Now(), tt.tv_sec, tt.tv_nsec);
            ret = pthread_cond_timedwait ( &Dcond, &Dlock, &tt );
            // fprintf(stderr, "volvi del tuto con %d\n", ret);
        }

        /* Revisar clientes: timeouts y datos entrantes */

        if ( connection.state == FREE ) continue;

        if ( rc == INTERR_RSEND && connection.lar != connection.lfs ) /* retransmitir paquetes */
        {
            fprintf(stderr, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA %d\n", rc);
            if ( Data_debug ) fprintf ( stderr, "Retransmitir!\n" );

            for ( i = 0; i < MAX_SQN + 1; i++ )
            {
                if ( connection.resend[i] )
                {
                    fprintf ( stderr, "Reenviar SEQN = %d\n", i );
                    sendPacket ( i ); /* reenvia el paquete i */
                }

            }

        }

        else if ( rc == INTERR_DATA ) /* data en la bufbox */
        {
            /* hay data en el buffer, y tengo espacio para enviar */
            sendPacket( -1 );
        }

        pthread_mutex_unlock ( &Dlock );
    }

    return NULL;
}

/* funcion auxiliar encargada de actualizar el RTT */
static void updRTT ( unsigned char seqn )
{
    double rtt_sample = Now() - connection.timestamp[seqn];

    if ( connection.mdev < 0 )
    {
        connection.rtt = rtt_sample;
        connection.mdev = connection.rtt / 2.0;

        if ( Data_debug )
            fprintf ( stderr, "ACTUALIZANDO  RTT = %f \n", connection.rtt );

        return;

    }


    double diff = rtt_sample - connection.rtt;
    double rtt = connection.rtt + ( ( 1.0 / 8.0 ) * diff );
    connection.mdev = ( 0.75 * connection.mdev ) + ( 0.25 * abs ( diff ) );

    connection.rtt = rtt + 4 * connection.mdev;

    if ( connection.rtt > 3.0 )
        connection.rtt = 3.0;

    else if ( connection.rtt < 0.05 )
        connection.rtt = 0.05;

    if ( Data_debug )
        fprintf ( stderr, "ACTUALIZANDO  RTT = %f \n", connection.rtt );

    return;
}

static void sendPacket( int seqn )
/* reenvia un paquete */
/* si seqn == -1, en vez de reenviar, envia un paquete por primera vez */
{
    unsigned char nlfs;

    if ( seqn == -1 )
    {
        nlfs = connection.lfs + 1;

        connection.pending_sz[nlfs] = getbox ( connection.wbox,
            ( char * ) connection.swindow[nlfs] + DHDR, BUF_SIZE );

        connection.swindow[nlfs][DID] = connection.id;
        connection.swindow[nlfs][DSEQ] = nlfs;
        connection.swindow[nlfs][DRTN] = 1;

        if ( connection.pending_sz[nlfs] == -1 ) /* eof */
        {
            connection.swindow[nlfs][DTYPE] = CLOSE;
            connection.state = CLOSED;
            connection.pending_sz[nlfs] = 0;
            connection.eof = 1;

            if ( Data_debug )
                fprintf ( stderr, "Enviando eof\n");
        }
        else
        {
            connection.swindow[nlfs][DTYPE] = DATA;

            if ( Data_debug )
                fprintf ( stderr, "Enviando Paquete: seqn = %d, lfs = %d\n", nlfs, nlfs );
        }

        send ( Dsock, connection.swindow[nlfs], connection.pending_sz[nlfs] + DHDR, 0 );

        connection.timestamp[nlfs] = Now();
        connection.timeout[nlfs] = Now() + connection.rtt;
        connection.retries[nlfs] = 1;
        connection.resend[nlfs] = 0;
        connection.exp_ack[nlfs] = 1;
        connection.lfs = nlfs;

        return;
    }

    if ( connection.retries[seqn]++ > RETRIES )
    {
        fprintf ( stderr, "Too many retries.\n");
        del_connection();
        exit(1);
    }

    connection.swindow[seqn][DRTN] = connection.retries[seqn];

    if ( Data_debug )
        fprintf ( stderr, "Reenviando Paquete: seqn = %d, lfs = %d, retries = %d\n", seqn, connection.lfs, connection.retries[seqn] );

    send ( Dsock, connection.swindow[seqn], connection.pending_sz[seqn] + DHDR, 0 );

    connection.timestamp[seqn] = Now();
    connection.timeout[seqn] = Now() + connection.rtt;

    connection.resend[seqn] = 0;
    connection.exp_ack[seqn] = 1;
}

static int in_SWindow ( unsigned char seqn )
/* verifica que seqn este en la ventana de envio */
{
    if ( connection.lfs > connection.lar )
    {
        if ( seqn > connection.lar && seqn <= connection.lfs )
            return 1;
        else
            return 0;
    }
    else
    {
        if ( seqn > connection.lar || seqn <= connection.lfs )
            return 1;
        else
            return 0;
    }
}

static int in_RWindow ( unsigned char seqn )
/* verifica que seqn este en la ventana de recepcion */
{
    if ( connection.laf > connection.lfr )
    {
        if ( seqn > connection.lfr && seqn <= connection.laf )
            return 1;
        else
            return 0;
    }
    else
    {
        if ( seqn > connection.lfr || seqn <= connection.laf )
            return 1;
        else
            return 0;
    }
}
