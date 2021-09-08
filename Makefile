COURSE = /Users/rhuck/documents/compProjects/Terminal_Text

CC = gcc
CFLAGS = -std=gnu11 -Wall -Wextra -Werror -I${COURSE}/include -g
LDFLAGS = -lpthread #-lnsl -lrt -lresolv

PROG = terminalText
OBJS = terminalText.o csapp.o

all: terminalText

terminalText: ${OBJS}
	${CC} ${CFLAGS} -v -o ${PROG} ${OBJS} ${LDFLAGS}

terminalText.o: terminalText.c ${COURSE}/include/csapp.h
	${CC} ${CFLAGS} -c terminalText.c

csapp.o: ${COURSE}/src/csapp.c ${COURSE}/include/csapp.h
	${CC} ${CFLAGS} -c ${COURSE}/src/csapp.c -o csapp.o

clean:
	${RM} *.o terminalText core.[1-9]*

.PHONY: all clean
