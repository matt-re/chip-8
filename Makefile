CC	:= clang
CFLAGS	:= -std=c23		\
	   -O2			\
	   -fstrict-aliasing	\
	   -pedantic		\
	   -Wall		\
	   -Werror		\
	   -Wextra		\
	   -Wshadow		\
	   -Wconversion		\
	   -Wstrict-aliasing

LDFLAGS	:=

TARGET	:= chip8
OBJS	:= chip8.o

.PHONY: all clean run

all: $(TARGET)

%.o: %.c data.h
	$(CC) -c $(CFLAGS) $<

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

clean:
	@rm -f $(TARGET) $(OBJS)

run: $(TARGET)
	./$(TARGET)
