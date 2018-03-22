CC=gcc
CFLAGS=-Wall -O2
LDFLAGS=-Wall -laio -lm

LUA_CFLAGS      != pkg-config --cflags lua5.2
LUA_LIBS        != pkg-config --libs lua5.2

ZERO_EXE=zero_md0
ZERO_SRC=zero_md0.c
ZERO_OBJ=$(ZERO_SRC:.c=.o)

LFOLAYS_LIB=folays.so
LFOLAYS_SRC=lfolays.c
LFOLAYS_OBJ=$(LFOLAYS_SRC:.c=.o)

all: $(ZERO_EXE) $(LFOLAYS_LIB)

$(ZERO_EXE): $(ZERO_OBJ)
	$(CC) $(ZERO_OBJ) $(LDFLAGS) -o $(ZERO_EXE)

$(LFOLAYS_LIB): $(LFOLAYS_OBJ)
	$(CC) -o $@ -shared $(LDFLAGS) $(LUA_LIBS) $(LFOLAYS_OBJ)

$(LFOLAYS_OBJ): $(LFOLAYS_SRC)
	$(CC) -o $@ $(LUA_CFLAGS) -fPIC -c $(LFOLAYS_SRC)

clean:
	rm -f $(ZERO_OBJ) $(LFOLAYS_OBJ)

distclean: clean
	rm -f $(ZERO_EXE) $(LFOLAYS_LIB)
