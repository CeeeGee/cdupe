CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11
TARGET  = cdupe

$(TARGET): src/main.c
	$(CC) $(CFLAGS) -o $(TARGET) src/main.c

clean:
	rm -f $(TARGET)