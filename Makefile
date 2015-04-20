CC=g++

WORKROOT=..
THIRD=$(WORKROOT)/thirdparty/output

TARGET=tiny_op case_op

LIBEVENT=$(THIRD)/libevent
LIBCURL=$(THIRD)/libcurl
JSONCPP=$(THIRD)/jsoncpp
OPENSSL=$(THIRD)/openssl
LIBIDN=$(THIRD)/libidn
MYLOG=$(THIRD)/mylog
ZLIB=$(THIRD)/zlib

INCLUDES=-I$(LIBEVENT)/include \
		 -I$(LIBCURL)/include \
		 -I$(JSONCPP)/include \
		 -I$(OPENSSL)/include \
		 -I$(LIBIDN)/include \
		 -I$(MYLOG)/include \
		 -I$(ZLIB)/include

LIBS=-L$(LIBEVENT)/lib \
	 -L$(LIBCURL)/lib \
	 -L$(JSONCPP)/lib \
	 -L$(OPENSSL)/lib \
	 -L$(LIBIDN)/lib \
	 -L$(MYLOG)/lib \
	 -L$(ZLIB)/lib

LDFLAGS=-Wl,--dn -levent -lmylog -pthread -lcurl -lidn -lssl -lcrypto -lz -ljsoncpp \
		-Wl,--dy -lrt -ldl

CFLAGS=-fPIC -g -finline-functions -Wall -Werror

.PHONY: all clean

all: $(TARGET)

clean:
	rm -rf *.o $(TARGET) log

tiny_op: tiny_op.o
	$(CC) $(LIBS) $^ $(LDFLAGS) -o $@

case_op: case_op.o
	$(CC) $(LIBS) $^ $(LDFLAGS) -o $@

%.o	: %.cpp
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@
