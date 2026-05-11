CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -pedantic -D_POSIX_C_SOURCE=200809L
TARGET = myinit

.PHONY: all clean

all: $(TARGET)

$(TARGET): myinit.o
	$(CC) $(CFLAGS) -o $(TARGET) myinit.o

myinit.o: myinit.c
	$(CC) $(CFLAGS) -c myinit.c

clean:
	rm -f $(TARGET) *.o result.txt
