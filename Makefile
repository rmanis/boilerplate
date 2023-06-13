CC=gcc
CFLAGS=-c -g -Isrc

SRCDIR=src
O=obj

PRODUCTS=server server-list server-pty

default:
	@echo Possible targets:
	@echo $(PRODUCTS) | column
.PHONY: default

$O/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	find $O -name '*.dSYM' -exec rm -r {} \;
	find $O -type f -delete
	rm -f $(PRODUCTS)
.PHONY: clean

server: $O/net/server.o
	$(CC) -o $@ $^

server-list: $O/net/server-list.o $O/util/list.o
	$(CC) -o $@ $^

server-pty: $O/net/server-pty.o $O/util/list.o
	$(CC) -o $@ $^

