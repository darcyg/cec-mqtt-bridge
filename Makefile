CC := $(CROSS_COMPILE)gcc

CFLAGS =
CFLAGS += -Wall
CFLAGS += -pedantic
CFLAGS += --std=c11

# Edit this to point to the correct ceccloader.h
CFLAGS += -I$(HOME)/src/libcec/include

LDFLAGS =
LDFLAGS += -ldl
LDFLAGS += -lcec
LDFLAGS += -lmosquitto
LDFLAGS += -ljansson

EXEC := cec-mqtt-bridge

OBJS =

OBJS += src/main.o

.PRECIOUS: %.o

all: $(EXEC) 

debug: CFLAGS += -ggdb
debug: $(EXEC)

%.o : %.c
	$(CC) $(CFLAGS) -o $@ -c $<

$(EXEC): $(OBJS)
	$(CC) $(CFLAGS) -o $(EXEC) $(OBJS) $(LDFLAGS) 

clean:
	rm -f src/*.o
	if test -f ${EXEC}; then rm ${EXEC}; fi
	if test -d ${EXEC}.dSYM; then rm -rf ${EXEC}.dSYM; fi

install: $(EXEC)
	install ${EXEC} /usr/local/bin
