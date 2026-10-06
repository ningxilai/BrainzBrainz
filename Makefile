CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Iinclude

BINS = musicbrainz bookbrainz

all: $(BINS)

musicbrainz: src/musicbrainz.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

bookbrainz: src/bookbrainz.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(BINS)

.PHONY: all clean
