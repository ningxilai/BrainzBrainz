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

# Live smoke tests (need network access to musicbrainz.org / api.bookbrainz.org).
test: $(BINS)
	./musicbrainz search artist --query "artist:radiohead" --limit 1 | grep -q Radiohead
	./musicbrainz lookup release 4b3d18cc-8937-36f4-8de0-481088be58e6 | grep -q Airbag
	./musicbrainz browse release --artist a74b1b7f-71a5-4011-9441-d0b5e4122711 --limit 1 | grep -q "of "
	! ./musicbrainz lookup artist a74b1b7f-71a5-4011-9441-d0b5e4122711 --inc bogus-thing 2>/dev/null
	./musicbrainz search artist --query "artist:radiohead" --limit 1 --json | python3 -c "import sys,json; assert json.load(sys.stdin)['artists'][0]['name']=='Radiohead'"
	./bookbrainz search author --query "Tolkien" --limit 1 | grep -qi tolkien
	./bookbrainz lookup edition 2c389e5c-cf78-449e-9f9e-a5f7840b085b | grep -q Dune
	./bookbrainz browse edition --author d0ac6e1f-617d-41a7-928a-26bb401f77ad --limit 1 | grep -q "of "
	! ./bookbrainz lookup author d0ac6e1f-617d-41a7-928a-26bb401f77ad --inc bogus-thing 2>/dev/null
	@echo "smoke: all passed"

.PHONY: all clean test
