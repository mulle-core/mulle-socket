/*
 * timeout-probe.c -- what a socket timeout actually does on this platform.
 *
 * Three questions the documentation does not answer the same way twice:
 *
 *   1. a read that runs out of time: which error, and after how long?
 *   2. a write that runs out of time: the same two questions.
 *   3. does a receive timeout bound accept() as well, or only recv()?
 *
 * The order matters. Question 3 is asked last and announces itself first,
 * because it is the one a platform may simply not honour -- and a probe that
 * hangs before printing anything measures nothing. Run it under an external
 * bound (`timeout 30`) and the last line it printed is the answer.
 *
 * It is deliberately not in test/: on two of the three platforms the third
 * question hangs rather than fails, so the one thing it exists to find out is
 * the one thing a test could not assert. What it found is written down in
 * ../README.md and beside the timeout calls in ../src/mulle-socket.h; this is
 * what to run again when a platform's answer is in doubt.
 *
 *   cc -std=gnu11 -isystem <mulle-core>/src -o timeout-probe timeout-probe.c \
 *      -L<build>/mulle-core -lmulle-core -lpthread      # + -lws2_32 on Windows
 *   timeout 30 ./timeout-probe
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <mulle-socket/mulle-socket.h>

#ifdef _WIN32
# include <windows.h>
static double   now_seconds( void)   { return( (double) GetTickCount64() / 1000.0); }
#else
# include <time.h>
static double   now_seconds( void)
{
   struct timespec   ts;

   clock_gettime( CLOCK_MONOTONIC, &ts);
   return( (double) ts.tv_sec + (double) ts.tv_nsec / 1e9);
}
#endif


static char const   *error_name( int error)
{
   switch( error)
   {
   case 0           : return( "0");
   case ETIMEDOUT   : return( "ETIMEDOUT");
   case EAGAIN      : return( "EAGAIN/EAGAIN is the POSIX spelling here");
   case ECONNRESET  : return( "ECONNRESET");
   case EPIPE       : return( "EPIPE");
   default          : return( "other");
   }
}


int   main( void)
{
   mulle_socket_t   listener;
   mulle_socket_t   client;
   mulle_socket_t   server;
   unsigned int     port;
   double           started;
   double           elapsed;
   intptr_t         n;
   char             buffer[ 64];
   char             *big;

   /* unbuffered, so a hang still shows how far it got */
   setvbuf( stdout, NULL, _IONBF, 0);

   if( mulle_socket_init())
   {
      printf( "init failed\n");
      return( 1);
   }

   port     = 0;
   listener = mulle_socket_listen( "127.0.0.1", 0, &port);
   if( listener == MULLE_SOCKET_INVALID)
   {
      printf( "listen failed\n");
      return( 1);
   }

   client = mulle_socket_connect( "127.0.0.1", port);
   if( client == MULLE_SOCKET_INVALID)
   {
      printf( "connect failed\n");
      return( 1);
   }

   server = mulle_socket_accept( listener);
   if( server == MULLE_SOCKET_INVALID)
   {
      printf( "accept failed\n");
      return( 1);
   }
   printf( "connected\n");

   /* ------------------------------------------------------------- 1. read */

   if( mulle_socket_set_read_timeout( client, 0.25))
      printf( "read: setting the timeout failed\n");

   started = now_seconds();
   n       = mulle_socket_read( client, buffer, sizeof( buffer));
   elapsed = now_seconds() - started;

   printf( "read:  returned %ld after %.3fs, error %s\n",
           (long) n, elapsed, error_name( mulle_socket_error()));

   /* ------------------------------------------------------------- 2. write */
   /*
    * Nobody reads `server`, so the sends fill its receive window and then this
    * socket's send buffer, and the next one blocks. 8 MB is past every default.
    */
   if( mulle_socket_set_write_timeout( client, 0.25))
      printf( "write: setting the timeout failed\n");

   big = malloc( 8 * 1024 * 1024);
   memset( big, 'x', 8 * 1024 * 1024);

   started = now_seconds();
   n       = mulle_socket_write_all( client, big, 8 * 1024 * 1024);
   elapsed = now_seconds() - started;

   printf( "write: returned %ld after %.3fs, error %s\n",
           (long) n, elapsed, error_name( mulle_socket_error()));
   free( big);

   mulle_socket_close( server);
   mulle_socket_close( client);
   mulle_socket_close( listener);

   /* ------------------------------------------------------------ 3. accept */
   /*
    * A fresh listener with nothing coming, and a receive timeout on it. If the
    * platform honours one here, this prints within ~0.3s; if it does not, the
    * line announcing it is the last thing the probe says.
    */
   port     = 0;
   listener = mulle_socket_listen( "127.0.0.1", 0, &port);
   printf( "accept: asking for a 0.3s bound on a listener with no clients\n");

   if( mulle_socket_set_read_timeout( listener, 0.3))
      printf( "accept: setting the timeout failed\n");

   started = now_seconds();
   n       = (intptr_t) mulle_socket_accept( listener);
   elapsed = now_seconds() - started;

   printf( "accept: %s after %.3fs, error %s\n",
           n == (intptr_t) MULLE_SOCKET_INVALID ? "failed" : "returned a socket",
           elapsed,
           error_name( mulle_socket_error()));

   mulle_socket_close( listener);
   return( 0);
}
