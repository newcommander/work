CC=g++

WORKROOT=..
THIRD=$(WORKROOT)/thirdparty/output

TARGET=tiny_op case_op reference decoder

LIBEVENT=$(THIRD)/libevent
LIBCURL=$(THIRD)/libcurl
JSONCPP=$(THIRD)/jsoncpp
OPENSSL=$(THIRD)/openssl
LIBIDN=$(THIRD)/libidn
FFMPEG=$(THIRD)/ffmpeg
MYLOG=$(THIRD)/mylog
ZLIB=$(THIRD)/zlib

INCLUDES=-I$(LIBEVENT)/include \
		 -I$(LIBCURL)/include \
		 -I$(JSONCPP)/include \
		 -I$(OPENSSL)/include \
		 -I$(LIBIDN)/include \
		 -I$(FFMPEG)/include \
		 -I$(MYLOG)/include \
		 -I$(ZLIB)/include

LIBS=-L$(LIBEVENT)/lib \
	 -L$(LIBCURL)/lib \
	 -L$(JSONCPP)/lib \
	 -L$(OPENSSL)/lib \
	 -L$(LIBIDN)/lib \
	 -L$(FFMPEG)/lib \
	 -L$(MYLOG)/lib \
	 -L$(ZLIB)/lib

LDFLAGS=-Wl,--dn -levent -lmylog -pthread -lcurl -lidn -lssl -lcrypto \
		-ljsoncpp -lswresample -lavdevice -lavfilter -lswscale \
		-lavformat -lavcodec -lswresample -lavutil -lz \
		-Wl,--dy -lrt -ldl -lxcb-shape -lxcb-shm -lxcb-xfixes

CFLAGS=-fPIC -g -finline-functions -Wall -Werror

.PHONY: all clean

all: $(TARGET)

clean:
	rm -rf *.o $(TARGET) log

tiny_op: tiny_op.o
	$(CC) $(LIBS) $^ $(LDFLAGS) -o $@

case_op: case_op.o logic.o
	$(CC) $(LIBS) $^ $(LDFLAGS) -o $@

reference: reference.o logic.o
	$(CC) $(LIBS) $^ $(LDFLAGS) -o $@

decoder: decoder.o
	$(CC) $(LIBS) $^ $(LDFLAGS) -o $@

%.o	: %.cpp
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@
