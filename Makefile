CC=/usr/bin/gcc
CC_OPTS=-g3
CC_LIBS=
CC_DEFINES=
CC_INCLUDES=
CC_ARGS=${CC_OPTS} ${CC_LIBS} ${CC_DEFINES} ${CC_INCLUDES}

# clean is not a file
.PHONY=clean


chord: chord.c
	@${CC} ${CC_ARGS} -o chord chord.c -lpthread

clean:
	@rm -f chord *.o
