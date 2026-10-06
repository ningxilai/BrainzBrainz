CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Iinclude

BIN = musicbrainz
SRC = src/musicbrainz.cpp

all: $(BIN)

$(BIN): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(BIN)

# Live smoke test (needs network access to musicbrainz.org).
test: $(BIN)
	./$(BIN) search artist --query "artist:radiohead" --limit 1 | grep -q Radiohead
	./$(BIN) lookup release 4b3d18cc-8937-36f4-8de0-481088be58e6 | grep -q Airbag
	./$(BIN) browse release --artist a74b1b7f-71a5-4011-9441-d0b5e4122711 --limit 1 | grep -q "of "
	! ./$(BIN) lookup artist a74b1b7f-71a5-4011-9441-d0b5e4122711 --inc bogus-thing 2>/dev/null
	./$(BIN) search artist --query "artist:radiohead" --limit 1 --json | python3 -c "import sys,json; assert json.load(sys.stdin)['artists'][0]['name']=='Radiohead'"
	@echo "smoke: all passed"

.PHONY: all clean test
