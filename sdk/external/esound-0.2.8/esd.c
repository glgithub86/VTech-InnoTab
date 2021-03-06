#include "esd-server.h"

#include <arpa/inet.h>

#ifndef HAVE_NANOSLEEP
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

/*******************************************************************/
/* esd.c - prototypes */
void set_audio_buffer( void *buf, esd_format_t format, int magl, int magr, 
		int freq, int speed, int length, long offset );
void clean_exit( int signum );
void reset_signal( int signum );
void reset_daemon( int signum );
int open_listen_socket( int port );

/*******************************************************************/
/* globals */

int esd_is_owned = 0;		/* start unowned, first client claims it */
int esd_is_locked = 1;		/* if owned, will prohibit foreign sources */
char esd_owner_key[ESD_KEY_LEN]; /* the key that locks the daemon */

int esd_on_standby = 0;		/* set to route ignore incoming audio data */
int esdbg_trace = 0;		/* show warm fuzzy debug messages */
int esdbg_comms = 0;		/* show protocol level debug messages */
int esdbg_mixer = 0;		/* show mixer engine debug messages */

int esd_buf_size_octets = 0; 	/* size of audio buffer in bytes */
int esd_buf_size_samples = 0; 	/* size of audio buffer in samples */
int esd_sample_size = 0;	/* size of sample in bytes */

int esd_beeps = 1;		/* whether or not to beep on startup */
int listen_socket = -1;		/* socket to accept connections on */

int esd_autostandby_secs = -1; 	/* timeout to release audio device, disabled <0 */
time_t esd_last_activity = 0;	/* seconds since last activity */
int esd_on_autostandby = 0;	/* set when auto paused for auto reawaken */


/*******************************************************************/
/* just to create the startup tones for the fun of it */
void set_audio_buffer( void *buf, esd_format_t format,
		       int magl, int magr, 
		       int freq, int speed, int length, long offset )
{
    int i;
    float sample;
    float kf = 2.0 * 3.14 * (float)freq / (float)speed;

    unsigned char *uc_buf = (unsigned char *)buf;
    signed short *ss_buf = (signed short *)buf;
    
    /* printf( "fmt=%d, ml=%d, mr=%d, freq=%d, speed=%d, len=%ld\n",
       format, magl, magr, freq, speed, length ); */

    switch ( format & ESD_MASK_BITS )
    {
    case ESD_BITS8:
	for ( i = 0 ; i < length ; i+=2 ) {
	    sample = sin( (float)(i+offset) * kf );
	    uc_buf[i] = 127 + magl * sample;
	    uc_buf[i+1] = 127 + magr * sample;
	}
	break;
    case ESD_BITS16:	/* assume same endian */
	for ( i = 0 ; i < length ; i+=2 ) {
	    sample = sin( (float)(i+offset) * kf );
	    ss_buf[i] = magl * sample;
	    ss_buf[i+1] = magr * sample;
	}
	break;
    default:
	fprintf( stderr, 
		 "unsupported format for set_audio_buffer: 0x%08x\n", 
		 format );
	exit( 1 );
    }


    return;
}

/*******************************************************************/
/* to properly handle signals */

void reset_daemon( int signum )
{
    int tumbler;

    ESDBG_TRACE( 
	printf( "(ca) resetting sound daemon on signal %d\n",
		signum ); );

    /* reset the access rights */
    esd_is_owned = 0;
    esd_is_locked = 1;

    /* scramble the stored key */
    srand( time(NULL) );
    for ( tumbler = 0 ; tumbler < ESD_KEY_LEN ; tumbler++ ) {
	esd_owner_key[ tumbler ] = rand() % 256;
    }

    /* close the clients */
    while ( esd_clients_list != NULL )
    {
	erase_client( esd_clients_list );
    }

    /* free samples */
    while ( esd_samples_list != NULL )
    {
	erase_sample( esd_samples_list->sample_id, 1 );
	/* TODO: kill_sample, so it stops playing */
	/* a looping sample will get stuck */
    }

    /* reset next sample id */
    esd_next_sample_id = 1;

    /* reset signal handler, if not called from a signal, no effect */
    signal( SIGHUP, reset_daemon );
}

void clean_exit(int signum) {
    /* just clean up as best we can and terminate from here */
    esd_client_t * client = esd_clients_list;
    
    fprintf( stderr, "received signal %d: terminating...\n", signum );
    
    /* free the sound device */
    esd_audio_close();

    /* close the listening socket */
    close( listen_socket );

    /* close the clients */
    while ( client != NULL )
    {
	close( client->fd );
	client = client->next;
    }

    /* trust the os to clean up the memory for the samples and such */
    fprintf( stderr, "bye bye.\n" );
    exit( 0 );
}

void reset_signal(int signum) {
    fprintf( stderr, "received signal %d: resetting...\n", signum );
    signal( signum, reset_signal);

    return;
}

/*******************************************************************/
/* returns the listening socket descriptor */
int open_listen_socket( int port )
{
    /*********************/
    /* socket test setup */
    struct sockaddr_in socket_addr;
    int socket_listen;
    struct linger lin;
   
    /* create the socket, and set for non-blocking */
    socket_listen=socket(AF_INET,SOCK_STREAM,0);
    if (socket_listen<0) 
    {
	fprintf(stderr,"Unable to create socket\n");
	return( -1 );
    }
    if (fcntl(socket_listen,F_SETFL,O_NONBLOCK)<0)
    {
	fprintf(stderr,"Unable to set socket to non-blocking\n");
	return( -1 );
    }

    /* set socket for linger? */
    lin.l_onoff=1;	/* block a closing socket for 1 second */
    lin.l_linger=100;	/* if data is waiting to be sent */
    if ( setsockopt( socket_listen, SOL_SOCKET, SO_LINGER,
		     &lin, sizeof(struct linger) ) < 0 ) 
    {
	fprintf(stderr,"Unable to set socket linger value to %d\n",
		lin.l_linger);
	return( -1 );
    }
    {
      int n = 1;
      setsockopt(socket_listen, SOL_SOCKET, SO_REUSEADDR, &n, sizeof(n));
      /* if it fails, so what */
    }

    /* set the listening information */
    socket_addr.sin_family = AF_INET;
    socket_addr.sin_port = htons( port );
    socket_addr.sin_addr.s_addr = htonl( inet_addr("0.0.0.0") );
    if ( bind( socket_listen,
	       (struct sockaddr *) &socket_addr,
	       sizeof(struct sockaddr_in) ) < 0 )
    {
	fprintf(stderr,"Unable to bind port %d\n", port );
	exit(1);
    }
    if (listen(socket_listen,16)<0)
    {
	fprintf(stderr,"Unable to set socket listen buffer length\n");
	exit(1);
    }

    return socket_listen;
}

/*******************************************************************/
/* daemon eats sound data, without playing anything, return boolean ok */
int esd_server_standby(void)
{
    int ok = 1;

    /* only bother if we're not already on standby */
    if ( !esd_on_standby ) {
	ESDBG_TRACE( printf( "setting sound daemon to standby\n" ); );
	
	/* TODO: close down any recorders, too */
	esd_on_standby = 1;
	esd_audio_close();
    }	

    return ok;
}

/*******************************************************************/
/* daemon goes back to playing sound data, returns boolean ok */
int esd_server_resume(void)
{
    int ok = 1;

    /* only bother if we're already on standby */
    if ( esd_on_standby ) {
	
	ESDBG_TRACE( printf( "resuming sound daemon\n" ); );
	
	/* reclaim the audio device */
	if ( esd_audio_open() < 0 ) {
	    /* device was busy or something, return error, try  later */
	    ok = 0;
	} else {
	    /* turn ourselves back on */
	    esd_on_standby = 0;
	    esd_on_autostandby = 0;
	}
    }

    return ok;
}

/*******************************************************************/
int main ( int argc, char *argv[] )
{
    /***************************/
    /* Enlightened sound Daemon */

    int esd_port = ESD_DEFAULT_PORT;
    int length = 0;
    int arg = 0;

    void *output_buffer = NULL;

    /* begin test scaffolding parameters */
    /* int format = AFMT_U8; AFMT_S16_LE; */
    /* int stereo = 0; */     /* 0=mono, 1=stereo */
    int default_rate = ESD_DEFAULT_RATE, default_buf_size = ESD_BUF_SIZE;
    int i, j, freq=440;
    int magl, magr;

    int default_format = ESD_BITS16 | ESD_STEREO;
    /* end test scaffolding parameters */

    /* parse the command line args */
    for ( arg = 1 ; arg < argc ; arg++ ) {
	if ( !strcmp( argv[ arg ], "-d" ) ) {
	    if ( ++arg != argc ) {
		esd_audio_device = argv[ arg ];
		if ( !esd_audio_device ) {
		    esd_port = ESD_DEFAULT_PORT;
		    fprintf( stderr, "- could not read device: %s\n", 
			     argv[ arg ] );
		}
		fprintf( stderr, "- using device %s\n", 
			 esd_audio_device );
	    }
	} else if ( !strcmp( argv[ arg ], "-port" ) ) {
	    if ( ++arg != argc ) {
		esd_port = atoi( argv[ arg ] );
		if ( !esd_port ) {
		    esd_port = ESD_DEFAULT_PORT;
		    fprintf( stderr, "- could not read port: %s\n", 
			     argv[ arg ] );
		}
		fprintf( stderr, "- accepting connections on port %d\n", 
			 esd_port );
	    }
	} else if ( !strcmp( argv[ arg ], "-b" ) ) {
	    fprintf( stderr, "- server format: 8 bit samples\n" );
	    default_format &= ~ESD_MASK_BITS; default_format |= ESD_BITS8; 
	} else if ( !strcmp( argv[ arg ], "-r" ) ) {
	    if ( ++arg != argc ) {
		default_rate = atoi( argv[ arg ] );
		if ( !default_rate ) {
		    default_rate = ESD_DEFAULT_RATE;
		    fprintf( stderr, "- could not read rate: %s\n", 
			     argv[ arg ] );
		}
		fprintf( stderr, "- server format: sample rate = %d Hz\n", 
			 default_rate );
	    }
	} else if ( !strcmp( argv[ arg ], "-as" ) ) {
	    if ( ++arg != argc ) {
		esd_autostandby_secs = atoi( argv[ arg ] );
		if ( !esd_autostandby_secs ) {
		    esd_autostandby_secs = ESD_DEFAULT_AUTOSTANDBY_SECS;
		    fprintf( stderr, "- could not read autostandby timeout: %s\n", 
			     argv[ arg ] );
		}
		fprintf( stderr, "- autostandby timeout: %d seconds\n", 
			 esd_autostandby_secs );
	    }
#ifdef ESDBG
	} else if ( !strcmp( argv[ arg ], "-vt" ) ) {
	    esdbg_trace = 1;
	    fprintf( stderr, "- enabling trace diagnostic info\n" );
	} else if ( !strcmp( argv[ arg ], "-vc" ) ) {
	    esdbg_comms = 1;
	    fprintf( stderr, "- enabling comms diagnostic info\n" );
	} else if ( !strcmp( argv[ arg ], "-vm" ) ) {
	    esdbg_mixer = 1;
	    fprintf( stderr, "- enabling mixer diagnostic info\n" );
#endif
	} else if ( !strcmp( argv[ arg ], "-nobeeps" ) ) {
	    esd_beeps = 0;
	    fprintf( stderr, "- disabling startup beeps\n" );
	} else if ( !strcmp( argv[ arg ], "-h" ) ) {
	    fprintf( stderr, "Usage: esd [options]\n\n" );
	    fprintf( stderr, "  -d DEVICE   force esd to use sound device DEVICE\n" );
	    fprintf( stderr, "  -b          run server in 8 bit sound mode\n" );
	    fprintf( stderr, "  -r RATE     run server at sample rate of RATE\n" );
	    fprintf( stderr, "  -as SECS    free audio device after SECS of inactivity\n" );
#ifdef ESDBG
	    fprintf( stderr, "  -vt         enable trace diagnostic info\n" );
	    fprintf( stderr, "  -vc         enable comms diagnostic info\n" );
	    fprintf( stderr, "  -vm         enable mixer diagnostic info\n" );
#endif
	    fprintf( stderr, "  -port PORT  listen for connections at PORT\n" );
	    fprintf( stderr, "\nPossible devices are:  %s\n", esd_audio_devices() );
	    exit( 0 );
	} else {
	    fprintf( stderr, "unrecognized option: %s\n", argv[ arg ] );
	}
    }

    /* start the initializatin process */
    printf( "initializing...\n" );

    /* set the data size parameters */
    esd_audio_format = default_format;
    esd_audio_rate = default_rate;

    esd_sample_size = ( (esd_audio_format & ESD_MASK_BITS) == ESD_BITS16 )
	? sizeof(signed short) : sizeof(unsigned char);
    esd_buf_size_samples = default_buf_size / 2;
    esd_buf_size_octets = esd_buf_size_samples * esd_sample_size;

    /* open and initialize the audio device, /dev/dsp */
    if ( esd_audio_open() < 0 ) {
	fprintf( stderr, "fatal error configuring sound, %s\n", 
		 "/dev/dsp" );
	exit( 1 );	    
    }

    /* allocate and zero out buffer */
    output_buffer = (void *) malloc( esd_buf_size_octets );
    memset( output_buffer, 0, esd_buf_size_octets );

    /* open the listening socket */
    listen_socket = open_listen_socket( esd_port );
    if ( listen_socket < 0 ) {
	fprintf( stderr, "fatal error opening socket\n" );
	exit( 1 );	    
    }
    
    /* install signal handlers for program integrity */
    signal( SIGINT, clean_exit );	/* for ^C */
    signal( SIGTERM, clean_exit );	/* for default kill */
    signal( SIGPIPE, reset_signal );	/* for closed rec/mon clients */
    signal( SIGHUP, reset_daemon );	/* kill -HUP clear ownership */

    /* send some sine waves just to check the sound connection */
    i = 0;
    if ( esd_beeps ) {
	magl = magr = ( (esd_audio_format & ESD_MASK_BITS) == ESD_BITS16) 
	    ? 30000 : 100;

	for ( freq = 55 ; freq < esd_audio_rate/2 ; freq *= 2, i++ ) {
	    /* repeat the freq for a few buffer lengths */
	    for ( j = 0 ; j < esd_audio_rate / 4 / esd_buf_size_samples ; j++ ) {
		set_audio_buffer( output_buffer, esd_audio_format, 
				  ( (i%2) ? magl : 0 ),  ( (i%2) ? 0 : magr ),
				  freq, esd_audio_rate, esd_buf_size_samples, 
				  j * esd_buf_size_samples );
		esd_audio_write( output_buffer, esd_buf_size_octets );
	    }
	}
    }

    /* pause the sound output */
    esd_audio_pause();

    /* until we kill the daemon */
    while ( 1 )
    {
	/* block while waiting for more clients and new data */
	wait_for_clients_and_data( listen_socket );

	/* accept new connections */
	get_new_clients( listen_socket );

	/* check for new protocol requests */
	poll_client_requests();

	/* mix new requests, and output to device */
	refresh_mix_funcs(); /* TODO: set a flag to cue when to do this */
	length = mix_players( output_buffer, esd_buf_size_octets );
	
	/* awaken if on autostandby and doing anything */
	if ( esd_on_autostandby && length && !esd_forced_standby ) {
	    ESDBG_TRACE( printf( "stuff to play, waking up.\n" ); );
	    esd_server_resume();
	}

	/* we handle this even when length == 0 because a filter could have
	 * closed, and we don't want to eat the processor if one did.. */
	if ( esd_filter_list && !esd_on_standby ) {
	    length = filter_write( output_buffer, length, 
				   esd_audio_format, esd_audio_rate );
	}
	
	if ( length > 0 /* || esd_monitor */ ) {
	    /* do_sleep = 0; */ 
	    if ( !esd_on_standby ) {
		/* standby check goes in here, so esd will eat sound data */
		/* TODO: eat a round of data with a better algorithm */
		/*        this will cause guaranteed timing issues */
		/* TODO: on monitor, why isn't this a buffer of zeroes? */
		/* esd_audio_write( output_buffer, esd_buf_size_octets ); */
		esd_audio_write( output_buffer, length );
		/* esd_audio_flush(); */ /* this is overkill */
		esd_last_activity = time( NULL );
	    }
	} else {
	    /* should be pausing just fine within wait_for_clients_and_data */
	    /* if so, this isn't really needed */

	    /* be very quiet, and wait for a wabbit to come along */
#if 0
	    if ( !do_sleep ) { ESDBG_TRACE( printf( "pausing in esd.c\n" ); ); }
	    do_sleep = 1;
	    esd_audio_pause();
#endif
	}

	/* if someone's monitoring the sound stream, send them data */
	/* mix_players, above, forces buffer to zero if no players */
	/* this clears out any leftovers from recording, below */
	if ( esd_monitor_list && !esd_on_standby && length ) {
	/* if ( esd_monitor_list && !esd_on_standby ) {  */
	    monitor_write( output_buffer, length );
	}

	/* if someone's recording the sound stream, send them data */
	if ( esd_recorder && !esd_on_standby ) { 
	    length = esd_audio_read( output_buffer, esd_buf_size_octets );
	    if ( length ) {
		length = recorder_write( output_buffer, length ); 
		esd_last_activity = time( NULL );
	    }
	}

	if ( esd_on_standby ) {
#ifdef HAVE_NANOSLEEP
	    struct timespec restrain;
	    restrain.tv_sec = 0;
	    /* funky math to make sure a long can hold it all, calulate in ms */
	    restrain.tv_nsec = (long) esd_buf_size_samples * 1000L
		/ (long) esd_audio_rate / 4L;   /* divide by two for stereo */
	    restrain.tv_nsec *= 1000000L;       /* convert to nanoseconds */
	    nanosleep( &restrain, NULL );
#else
	    struct timeval restrain;
	    restrain.tv_sec = 0;
	    /* funky math to make sure a long can hold it all, calulate in ms */
	    restrain.tv_usec = (long) esd_buf_size_samples * 1000L
		/ (long) esd_audio_rate / 4L; 	/* divide by two for stereo */
	    restrain.tv_usec *= 1000L; 		/* convert to microseconds */
	    select( 0, 0, 0, 0, &restrain );
#endif
	}
    } /* while ( 1 ) */

    /* how we'd get here, i have no idea, should only exit on signal */
    clean_exit( -1 );
    exit( 0 );
}
