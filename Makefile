# Para Linux agregar -lrt
CC=gcc
CFLAGS=-g # -m32

BIN= bwc-sr

all: $(BIN)

bwc-sr: bwc.o jsocket6.4.o Dataclient-SR.o jsocket6.4.h bufbox.o 
	$(CC) $(CFLAGS) bwc.o jsocket6.4.o Dataclient-SR.o bufbox.o -o $@ -lpthread -lrt

cleanall: 
	rm -f $(BIN) *.o
