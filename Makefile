CFLAGS = -DBIST -Wall -g -fPIC -I /usr/local/include -I /usr/local/include/tcl8.6/

libcindex.so: libcindex.o
	$(CC) -shared -o libcindex.so libcindex.o \
		-L /usr/local/lib -ltcl86 -lclang
	chmod -x libcindex.so

clean:
	rm -f libcindex.so libcindex.o
