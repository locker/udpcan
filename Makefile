CC = gcc
CFLAGS = -Wall -Werror

udpcan: udpcan.c
	$(CC) $(CFLAGS) -o $@ $^

PHONY += clean
clean:
	$(RM) udpcan

.PHONY: $(PHONY)
