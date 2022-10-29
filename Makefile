LD  := g++
CXX := g++

ARCH ?= native

LDFLAGS  += -static -flto
CXXFLAGS += -std=c++14 -Wall -Werror -pedantic -march=$(ARCH) -mtune=$(ARCH) -O2 -flto
CPPFLAGS += -D_FILE_OFFSET_BITS=64 -DRNG_CHACHA_ROUNDS=8

OBJ := main.o rng.o dev.o

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ -c $<

disktest: $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $^

.PHONY: clean
clean:
	rm -vf $(OBJ) disktest
