CC=gcc
CFLAGS=-Wall -O2
LDFLAGS=-Wall -laio -lm

ZERO_EXE=zero_md0
ZERO_SRC=zero_md0.c
ZERO_OBJ=$(ZERO_SRC:.c=.o)

LFOLAYS_LIB=folays.so
LFOLAYS_SRC=lfolays.c
LFOLAYS_OBJ=$(LFOLAYS_SRC:.c=.o)

all: $(ZERO_EXE)

$(ZERO_EXE): $(ZERO_OBJ)
	$(CC) $(ZERO_OBJ) $(LDFLAGS) -o $(ZERO_EXE)

$(LFOLAYS_LIB): $(LFOLAYS_OBJ)
	$(CC) $(LFOLAYS_OBJ) $(LDFLAGS) -o $(LFOLAYS_EXE)

clean:
	rm -f $(ZERO_OBJ) $(LFOLAYS_OBJ)

distclean: clean
	rm -f $(ZERO_EXE) $(LFOLAYS_LIB)
