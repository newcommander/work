#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <arpa/inet.h>

int show_init( char *ip, int port, int *screen_width, int *screen_height ){
    int sockfd, ret;
    unsigned char s[4];
    struct sockaddr_in servaddr;

    sockfd = socket( AF_INET, SOCK_STREAM, 0 );
    bzero( &servaddr, sizeof(servaddr) );
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons( port );
    inet_pton( AF_INET, ip, &servaddr.sin_addr );
    ret = connect( sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr) );
    if( ret != 0 ){
        printf( "connect failed. [%s]\n", strerror( errno ) );
        return -1;
    }

    read( sockfd, s, 4 );
    if( (screen_width != NULL) && (screen_height != NULL) ){
        *screen_width = s[0] + s[1]*0x100;
        *screen_height = s[2] + s[3]*0x100;
    }
    
    return sockfd;
}

int show_end( int sockfd ){
    close( sockfd );
    return 0;
}

int show( int sockfd, unsigned char *buf )
{
    int ret, length;

    if( sockfd == -1 ){
        printf( "socket error.\n" );
        return 1;
    }

    length = (unsigned int)(  (unsigned int)buf[0]
                            + (unsigned int)buf[1] * 0x100
                            + (unsigned int)buf[2] * 0x10000
                            + (unsigned int)buf[3] * 0x1000000 );
    length = length * 7 + 4;

    ret = 0;
    while( ret < length )
        ret += write( sockfd, buf+ret, length-ret );

    return 0;
}

int main_net( int argc, char **argv )
{
    int sockfd, i, j;
    int width, height;
    unsigned char *buf;
    if( argc < 3 ){
        printf( "Usage: %s ip port\n", argv[0] );
        return 1;
    }

    sockfd = show_init( argv[1], atoi( argv[2] ), &width, &height );
    if( sockfd == -1 ){
        printf( "Cannot connect to viewer.\n" );
        return 1;
    }

    buf = (unsigned char*)calloc( width*height*7+4, sizeof(unsigned char) );

    buf[0] = width*height & 0xff ;
    buf[1] = ( width*height >> 8 ) & 0xff;
    buf[2] = ( width*height >> 16 ) & 0xff;
    buf[3] = ( width*height >> 24 ) & 0xff;
    j = 1;
    while(1){
        for( i=0; i<width*height; i++ ){
            buf[ 4 + i*4 + 0 ] = ( i%width ) & 0xff;
            buf[ 4 + i*4 + 1 ] = ( ( i%width ) >> 8 ) & 0xff;
            buf[ 4 + i*4 + 2 ] = ( i/width ) & 0xff;
            buf[ 4 + i*4 + 3 ] = ( ( i/width ) >> 8 ) & 0xff;
            buf[ 4 + width*height*4 + i*3 + 0 ] = (j*10)%255;
            buf[ 4 + width*height*4 + i*3 + 1 ] = 0;
            buf[ 4 + width*height*4 + i*3 + 2 ] = 0;
        }
        //usleep( 10000 );
        show( sockfd, buf );
        printf( "done[%d]\n", j++ );
    }
    show_end( sockfd );

    return 0;
}
