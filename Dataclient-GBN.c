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
#include "Data-GBN.h"

#define MAX_QUEUE 100 /* buffers en boxes */

/* Version Go-Back-N */

void updRTT(); /* funcion para actualizar RTT y timeout */
void sendPacket();
void sendWindow();

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
    int pending_sz[MAX_SQN + 1];            /* tamano de los buffers esperando */
    int eof;                                /* eof */
    char last_ok_seq;                       /* ultimo seq correcto, para acks */
    int retries;                            /* cuantas veces he retransmitido */
    double timeout[MAX_SQN +
                   1];            /* tiempo restante antes de retransmision */
    double timestamp[MAX_SQN +
                     1];          /* marca cuando fue enviado el ultimo paquete */
    double rtt;                             /* RTT */
    double mdev;
    int state;                           /* FREE, CONNECTED, CLOSED */
    int id;                              /* id conexión, la asigna el servidor */
    unsigned char window[MAX_SQN + 1][BUF_SIZE]; /* ventana de envio */
    int lar;
    int lfs;
    int tout_cnt; /* contador de timeouts */
    int tmp_cnt;
    int f_resend; /* reenviar ventana */
    int f_packet; /* force send packet */
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
    int cl;
    pthread_t pid;
    int *p;

    connection.lar = -1;
    connection.lfs = -1;
    connection.tout_cnt = 0;
    connection.tmp_cnt = 0;
    connection.state = CONNECTED;
    connection.wbox = create_bufbox ( MAX_QUEUE );
    connection.rbox = create_bufbox ( MAX_QUEUE );
    connection.eof = 0;
    connection.last_ok_seq = -1;
    connection.id = id;
    connection.rtt = TIMEOUT;
    connection.mdev = -1;
    connection.f_resend = 0;
    connection.f_packet = 0;
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
    int cl, p, a;
    unsigned char inbuf[BUF_SIZE];
    int found;
    unsigned char exp_seq;

    /* Recibo paquete desde el socket */
    while ( ( cnt = recv ( Dsock, inbuf, BUF_SIZE, 0 ) ) > 0 )
    {
        exp_seq = ( connection.last_ok_seq + 1 ) % ( MAX_SQN + 1 );

        if ( Data_debug )
            fprintf ( stderr, "recv: id=%d, type=%c, seq=%d\n", inbuf[DID], inbuf[DTYPE],
                      inbuf[DSEQ] );

        if ( cnt < DHDR ) continue;

        cl = inbuf[DID];

        if ( cl != connection.id ) continue;

        pthread_mutex_lock ( &Dlock );

        if ( inbuf[DTYPE] == CLOSE )
        {
            if ( Data_debug )
                fprintf ( stderr, "recibo cierre conexión %d, envío ACK\n", cl );

            ack[DID] = cl;
            ack[DTYPE] = ACK;

            if ( inbuf[DSEQ] != exp_seq )
            {
                ack[DSEQ] = connection.last_ok_seq;

                if ( inbuf[DSEQ] > exp_seq )
                    ack[DRTN] = -1; /* NACK, apura fast retransmit */
                else
                    ack[DRTN] = 0;
            }

            else
            {
                ack[DSEQ] = exp_seq;
                ack[DRTN] = inbuf[DRTN];
            }


            if ( send ( Dsock, ack, DHDR, 0 ) < 0 )
            {
                perror ( "send" );
                exit ( 1 );
            }

            if ( inbuf[DSEQ] != exp_seq )
            {
                pthread_mutex_unlock ( &Dlock );
                continue;
            }

            connection.last_ok_seq = exp_seq;
            connection.state = CLOSED;
            Dclose ( cl );
        }

        else if ( inbuf[DTYPE] == ACK && connection.state != FREE )
        {
            /* verificamos que este dentro de la ventana y que coincidan
             * los numeros de reenvios*/

            if ( connection.lfs >= connection.lar )
            {
                a = inbuf[DSEQ] <= connection.lfs && inbuf[DSEQ] > connection.lar;
            }
            else
            {
                a = inbuf[DSEQ] <= connection.lfs || inbuf[DSEQ] > connection.lar;
            }

            fprintf(stderr, "a=%d\n", a);

            if ( a )
            {
                /* recibimos ack y ajustamos ventana */

                if ( Data_debug )
                    fprintf ( stderr, "recv ACK id=%d, LAR=%d\n LFS=%d\n", cl, inbuf[DSEQ], connection.lfs );

                connection.lar = inbuf[DSEQ];

                /* verificamos que el nro de retrans coincida y
                 * actualizamos el RTT */
                if ( connection.window[inbuf[DSEQ]][DRTN] == inbuf[DRTN] )
                    updRTT (); /* aqui actualizamos el RTT */

                if ( connection.state == CLOSED )
                {
                    /* conexion cerrada y sin buffers pendientes */
                    del_connection();
                }

                connection.f_packet = 1;

                pthread_cond_signal ( &Dcond );
            }

            else /* fast retransmit */
            {
                connection.tout_cnt++;

                if ( connection.tout_cnt >= 3 )
                    connection.f_resend = 1;

                pthread_cond_signal ( &Dcond );
            }

        }

        else if ( inbuf[DTYPE] == DATA && connection.state == CONNECTED )
        {
            if ( Data_debug ) fprintf ( stderr, "rcv: DATA: %d, seq=%d, expected=%d\n",
                                            inbuf[DID], inbuf[DSEQ], exp_seq );

            if ( boxsz ( connection.rbox ) >= MAX_QUEUE ) /* No tengo espacio */
            {
                pthread_mutex_unlock ( &Dlock );
                continue;
            }

            /* envio ack en todos los otros casos */
            ack[DID] = cl;
            ack[DTYPE] = ACK;

            if ( inbuf[DSEQ] != exp_seq )
            {
                ack[DSEQ] = connection.last_ok_seq;
                ack[DRTN] = 0;
            }

            else
            {
                ack[DSEQ] = exp_seq;
                ack[DRTN] = inbuf[DRTN];
            }

            if ( Data_debug ) fprintf ( stderr, "Enviando ACK %d, seq=%d\n", ack[DID],
                                            ack[DSEQ] );

            if ( send ( Dsock, ack, DHDR, 0 ) < 0 )
                perror ( "sendack" );

            if ( inbuf[DSEQ] != exp_seq )
            {
                pthread_mutex_unlock ( &Dlock );
                continue;
            }

            connection.last_ok_seq = exp_seq;
            /* enviar a la cola */
            putbox ( connection.rbox, ( char * ) inbuf + DHDR, cnt - DHDR );
        }

        else if ( Data_debug )
            fprintf ( stderr, "descarto paquete entrante: t=%c, id=%d\n", inbuf[DTYPE],
                      inbuf[DID] );

        pthread_mutex_unlock ( &Dlock );
    }

    fprintf ( stderr, "fallo read en Drcvr()\n" );
    return NULL;
}

double Dclient_timeout_or_pending_data()
{
    int cl, p, i;
    double timeout;
    /* Suponemos lock ya tomado! */

    timeout = Now() + 20.0;

    if ( connection.state == FREE ) return timeout;

    if ( boxsz ( connection.wbox ) != 0 || connection.f_packet )
        /* data from client */
        return Now();

    if ( connection.eof )
        return timeout;

    if ( connection.f_resend ) return Now(); /* fast retransmit! */

    /* a continuacion recorremos los timeouts
     * en efecto, retorna el timeout menor, o Now() si es que ya se
     * vencio el tiempo!*/
    if ( connection.lar <= connection.lfs )
    {
        for ( i = connection.lar + 1; i <= connection.lfs; i++ )
        {
            if ( connection.timeout[i] <= Now() )
            {
                connection.tout_cnt++;

                if ( connection.tout_cnt >= 3 )
                    connection.f_resend = 1;

                return Now();
            }

            if ( connection.timeout[i] < timeout )
                timeout = connection.timeout[i];

        }
    }

    else
    {
        for ( i = connection.lar + 1; i <= MAX_SQN; i++ )
        {
            if ( connection.timeout[i] <= Now() )
            {
                connection.tout_cnt++;

                if ( connection.tout_cnt >= 3 )
                    connection.f_resend = 1;

                return Now();
            }

            if ( connection.timeout[i] < timeout )
                timeout = connection.timeout[i];

        }

        for ( i = 0; i <= connection.lfs; i++ )
        {
            if ( connection.timeout[i] <= Now() )
            {
                connection.tout_cnt++;

                if ( connection.tout_cnt >= 3 )
                    connection.f_resend = 1;

                return Now();
            }

            if ( connection.timeout[i] < timeout )
                timeout = connection.timeout[i];
        }
    }

    return timeout;
}

/* Thread enviador y retransmisor */
static void *Dsender ( void *ppp )
{
    double timeout;
    struct timespec tt;
    int p;
    int ret;


    for ( ;; )
    {
        pthread_mutex_lock ( &Dlock );

        /* Esperar que pase algo */
        while ( ( timeout = Dclient_timeout_or_pending_data() ) > Now() )
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

        if ( connection.f_resend && !connection.f_packet ) /* retransmitir ventana */
        {

            if ( Data_debug ) fprintf ( stderr, "Retransmitir ventana\n" );

            if ( ++connection.retries > RETRIES )
            {
                fprintf ( stderr, "too many retries: %d\n", connection.retries );
                del_connection();
                exit ( 1 );
            }

            sendWindow(); /* reenviamos ventana completa */

            if ( Data_debug )
                fprintf ( stderr, "Retransmitiendo. Timeout = %f\n", connection.rtt );

            connection.tout_cnt = 0;
            connection.f_resend = 0;

        }

        else if ( boxsz ( connection.wbox ) != 0 ) /* && !connection.expecting_ack) */
        {
            /*
                    Hay un buffer para mi para enviar
                    leerlo, enviarlo, marcar esperando ACK
            */
            sendPacket();
        }

        pthread_mutex_unlock ( &Dlock );
    }

    return NULL;
}


void sendPacket()
{
    /* asumimos mutex! */

    int lfs = connection.lfs;
    int a;
    int i = connection.tmp_cnt;

    connection.f_packet = 0;
    connection.tout_cnt = 0;

    if ( lfs < 0 ) /* primer envio de la ventana */

        /* aqui guardamos paquetes hasta que la ventana este llena o
         * encontremos un EOF. Luego, enviamos la ventana completa. */
    {
        connection.pending_sz[i] = getbox ( connection.wbox,
                                            ( char * ) connection.window[i] + DHDR, BUF_SIZE );

        connection.window[i][DID] = connection.id;
        connection.window[i][DSEQ] = i;
        connection.window[i][DRTN] = 0;

        if ( connection.pending_sz[i] == -1 ) /* eof */
        {
            if ( Data_debug ) fprintf ( stderr,
                                            "enviando archivo completo en una ventana\n" );

            connection.state = CLOSED;
            connection.window[i][DTYPE] = CLOSE;
            connection.pending_sz[i] = 0;
            connection.eof = 1;


            sendWindow(); /* enviamos toda la ventana */
        }

        else
        {
            if ( Data_debug )
                fprintf ( stderr, "guardando DATA id=%d, seq=%d en ventana\n", connection.id,
                          connection.window[i][DSEQ] );

            connection.window[i][DTYPE] = DATA;
        }

        if ( i - connection.lar == WND_SIZE ) /* ventana llena, enviamos */
        {
            fprintf ( stderr, "Ventana llena, enviando\n" );
            sendWindow();
        }

        i++;
        connection.tmp_cnt = i;


    }

    else
    {

        if ( lfs >= connection.lar )
            a = lfs - connection.lar >= WND_SIZE;
        else
            a = lfs + MAX_SQN - connection.lar >= WND_SIZE;

        if ( ( a &&  connection.lar != connection.lfs) )
        {
            return; /* ventana llena */
        }


        lfs = ( lfs + 1 ) % ( MAX_SQN + 1 );

        connection.pending_sz[lfs] = getbox ( connection.wbox,
                                              ( char * ) connection.window[lfs] + DHDR, BUF_SIZE );

        connection.window[lfs][DID] = connection.id;
        connection.window[lfs][DSEQ] = lfs;
        connection.window[lfs][DRTN] = 0;

        if ( connection.pending_sz[lfs] == -1 ) /* EOF */
        {
            if ( Data_debug ) fprintf ( stderr, "sending EOF\n" );

            connection.state = CLOSED;
            connection.window[lfs][DTYPE] = CLOSE;
            connection.pending_sz[lfs] = 0;
        }

        else
        {
            if ( Data_debug )
                fprintf ( stderr, "sending DATA id=%d, seq=%d\n", connection.id,
                          connection.window[lfs][DSEQ] );

            connection.window[lfs][DTYPE] = DATA;
        }

        send ( Dsock, connection.window[lfs], DHDR + connection.pending_sz[lfs], 0 );

        /* aqui marcamos el envio del paquete */
        connection.timestamp[lfs] = Now();

        connection.timeout[lfs] = Now() + connection.rtt;

        connection.retries = 0;

        connection.lfs = lfs;
        if ( Data_debug )
            fprintf ( stderr, "LFS=%d\n", lfs );
    }
}


/* funcion auxiliar encargada de actualizar el RTT */
void updRTT ()
{
    double rtt_sample = Now() - connection.timestamp[connection.lar];

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

/* funcion encargada de enviar y reenviar la ventana completa */
void sendWindow ()
{
    /* asumimos mutex! */
    int i;

    if ( connection.lfs < 0 ) /* primer envio */
    {
        for ( i = connection.lar + 1; i <= connection.tmp_cnt; i++ )
        {
            connection.window[i][DRTN] += 1;
            send ( Dsock, connection.window[i], DHDR + connection.pending_sz[i], 0 );
            connection.timestamp[i] = Now();
            connection.timeout[i] = Now() + connection.rtt;
        }

        if ( Data_debug )
            fprintf ( stderr, "LFS luego de primer envio = %d\n", i - 1 );

        connection.lfs = i - 1;
        connection.tmp_cnt = -1;

    }

    else if ( connection.lfs >= connection.lar )
    {
        connection.rtt = connection.rtt * 2;

        if ( connection.rtt > TIMEOUT )
            connection.rtt = TIMEOUT;

        for ( i = connection.lar + 1; i <= connection.lfs; i++ )
        {
            connection.window[i][DRTN] += 1;
            send ( Dsock, connection.window[i], DHDR + connection.pending_sz[i], 0 );
            connection.timestamp[i] = Now();
            connection.timeout[i] = Now() + connection.rtt;
        }
    }

    else
    {
        connection.rtt = connection.rtt * 2;

        if ( connection.rtt > TIMEOUT )
            connection.rtt = TIMEOUT;

        for ( i = connection.lar + 1; i <= MAX_SQN; i++ )
        {
            connection.window[i][DRTN] += 1;
            send ( Dsock, connection.window[i], DHDR + connection.pending_sz[i], 0 );
            connection.timestamp[i] = Now();
            connection.timeout[i] = Now() + connection.rtt;
        }

        for ( i = 0; i <= connection.lfs; i++ )
        {
            connection.window[i][DRTN] += 1;
            send ( Dsock, connection.window[i], DHDR + connection.pending_sz[i], 0 );
            connection.timestamp[i] = Now();
            connection.timeout[i] = Now() + connection.rtt;
        }

    }
}
