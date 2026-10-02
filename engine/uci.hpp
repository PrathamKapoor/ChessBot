// UCI protocol loop.
//
// Minimal but complete enough to drive from a GUI or a match runner:
// uci, isready, ucinewgame, setoption, position, go, stop, quit, and the
// ponder/infine/infinite variants of go.
//
// OUTPUT RULES
// ------------
// bestmove must always be legal. A GUI cannot recover from an illegal bestmove,
// so when a search fails to produce one the loop emits the first legal move of
// the position instead, and only emits 0000 when there genuinely is none.
//
// info lines never contain a negative depth, a negative seldepth, or a score
// outside the mate sentinel range. The formatting helpers below clamp rather
// than trust the search, because a malformed info string makes a GUI drop the
// whole line and a GUI that has dropped its depth display is hard to debug from.
#pragma once

#include <atomic>
#include <iosfwd>
#include <string>
#include <thread>
#include <vector>

#include "position.hpp"
#include "search.hpp"

namespace pocket {

// Owns the game state the UCI loop manipulates and the options the search reads.
class UciEngine {
 public:
  UciEngine();

  // Reads commands until end of input.
  void loop(std::istream& in, std::ostream& out);

  // Handles a single command line. Returns false when the loop should stop.
  // Exposed so tests can drive the loop without a stream.
  bool execute(const std::string& line, std::ostream& out);

 private:
  void applyPosition(std::istream& in);
  void go(std::istream& in, std::ostream& out);
  void emitInfo(std::ostream& out, const SearchResult& r) const;

  // Runs one search to completion and prints info plus bestmove. Shared by the
  // synchronous and worker-thread paths so both emit identical output.
  void runSearch(const SearchLimits& limits, std::ostream& out);

  // Joins a search left over from a previous "go infinite" so that the position
  // is not mutated underneath a running thread.
  void finishWorker();

  Position pos_;
  bool havePosition_ = false;
  SearchOptions options_;

  std::thread worker_;
};

// Formatting helpers, exposed for testing.
std::string moveToUci(Move m);
Move moveFromUci(const std::string& s, bool* ok);

}  // namespace pocket