#include "uci.hpp"

#include <algorithm>
#include <istream>
#include <ostream>
#include <sstream>

#include "evaluate.hpp"
#include "tt.hpp"

namespace pocket {

// Formats a UCI move string: e2e4, e7e8q, e1g1 for castling, e5d6 for en
// passant. A null move becomes "0000".
std::string moveToUci(Move m) {
  if (m == kMoveNone) return "0000";
  std::string s;
  s += char('a' + int(fileOf(fromSq(m))));
  s += char('1' + int(rankOf(fromSq(m))));
  s += char('a' + int(fileOf(toSq(m))));
  s += char('1' + int(rankOf(toSq(m))));
  if (isPromotion(m)) {
    switch (promotionType(m)) {
      case kQueen: s += 'q'; break;
      case kRook: s += 'r'; break;
      case kBishop: s += 'b'; break;
      default: s += 'n'; break;
    }
  }
  return s;
}

Move moveFromUci(const std::string& s, bool* ok) {
  *ok = false;
  if (s.size() != 4 && s.size() != 5) return kMoveNone;
  const int ff = s[0] - 'a', fr = s[1] - '1';
  const int tf = s[2] - 'a', tr = s[3] - '1';
  if (ff < 0 || ff > 7 || fr < 0 || fr > 7 || tf < 0 || tf > 7 || tr < 0 || tr > 7)
    return kMoveNone;
  const Square from = makeSquare(File(ff), Rank(fr));
  const Square to = makeSquare(File(tf), Rank(tr));
  Move m = makeMove(from, to);
  if (s.size() == 5) {
    PieceType pt = kQueen;
    switch (s[4]) {
      case 'q': pt = kQueen; break;
      case 'r': pt = kRook; break;
      case 'b': pt = kBishop; break;
      case 'n': pt = kKnight; break;
      default: return kMoveNone;
    }
    m = makePromotion(from, to, pt);
  }
  *ok = true;
  return m;
}

namespace {

// Clamps a score into the range the protocol allows, so a corrupted value can
// never reach the output.
std::string formatScore(int score) {
  if (isMateScore(score)) {
    const int plies = mateDistanceInPlies(score);
    // Mate in 0 means the side to move is already mated, which UCI reports as
    // mate in 0. Signed by whether the score is positive.
    const int moves = (plies + 1) / 2;
    return std::string("mate ") + std::to_string(score > 0 ? moves : -moves);
  }
  const int clamped = std::max(-30000, std::min(30000, score));
  return "cp " + std::to_string(clamped);
}

std::string field(const std::string& key, const std::string& value) {
  return " " + key + " " + value;
}

const char* kEngineName = "PocketChess";

}  // namespace

UciEngine::UciEngine() = default;

void UciEngine::applyPosition(std::istream& in) {
  std::string token;
  in >> token;  // "startpos" or "fen"

  // The position is built with setFromFen on pos_ itself rather than assigned
  // from a temporary. Position holds a pointer into its own state stack, so a
  // default copy would leave that pointer aliasing the source object's stack.
  std::string err;
  if (token == "fen") {
    std::string fen, part;
    while (in >> part) {
      if (part == "moves") break;
      fen += (fen.empty() ? "" : " ") + part;
    }
    if (fen.empty() || !pos_.setFromFen(fen, &err)) return;  // keep the old position
  } else if (token == "startpos") {
    const char* kStart = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    if (!pos_.setFromFen(kStart, &err)) return;
    std::string part;
    while (in >> part)
      if (part == "moves") break;
  } else {
    return;  // unknown; leave the position alone
  }

  // Replay the move list. An illegal or unparseable move stops the replay where
  // it failed rather than corrupting the position.
  std::string part;
  if (in >> part) {
    if (part == "moves") {
      std::string ms;
      while (in >> ms) {
        bool ok = false;
        const Move m = moveFromUci(ms, &ok);
        if (!ok || !pos_.legal(m)) break;
        pos_.doMove(m);
      }
    }
  }

  havePosition_ = true;
}

void UciEngine::emitInfo(std::ostream& out, const SearchResult& r) const {
  // Time is reported as measured rather than rounded up to avoid a division by
  // zero: claiming a search took 1ms when it took none makes the nps figure a
  // fiction. Nodes divided by a zero interval is reported as 0, which is what it
  // is.
  const uint64_t elapsed = r.elapsedMs;
  const uint64_t nps = elapsed ? uint64_t(double(r.stats.nodes) * 1000.0 / double(elapsed)) : 0;

  std::ostringstream line;
  line << "info" << field("depth", std::to_string(std::max(1, r.depth)))
       << field("seldepth", std::to_string(std::max(1, r.seldepth)))
       << field("score", formatScore(r.score))
       << field("nodes", std::to_string(r.stats.nodes))
       << field("nps", std::to_string(nps))
       << field("time", std::to_string(elapsed));

  // The PV is emitted from the recorded line. Any move that is not legal from the
  // position it is played in is dropped rather than printed, so a stale PV cannot
  // produce a line a GUI would choke on.
  Position walk(pos_);
  std::ostringstream pv;
  bool any = false;
  for (Move m : r.pv) {
    if (!walk.legal(m)) break;
    if (any) pv << ' ';
    pv << moveToUci(m);
    walk.doMove(m);
    any = true;
  }
  if (any) line << field("pv", pv.str());
  out << line.str() << "\n";
}

void UciEngine::finishWorker() {
  // An infinite search only returns when it is asked to stop, so a join here
  // would deadlock on the next command unless the stop is issued first.
  if (worker_.joinable()) {
    g_searchStopRequested = true;
    worker_.join();
  }
}

void UciEngine::runSearch(const SearchLimits& limits, std::ostream& out) {
  Search search(pos_, options_);
  const SearchResult r = search.search(limits);

  // The search returns a legal move for any non-terminal position. This is
  // re-checked here rather than trusted, because an illegal bestmove is the one
  // output failure a GUI cannot recover from.
  Move best = r.best;
  if (best == kMoveNone || !pos_.legal(best)) {
    Move buf[256];
    int n = 0;
    generateLegal(pos_, buf, n);
    best = n > 0 ? buf[0] : kMoveNone;
  }

  emitInfo(out, r);
  out << "bestmove " << moveToUci(best) << "\n";
  out.flush();
}

void UciEngine::go(std::istream& in, std::ostream& out) {
  SearchLimits limits;
  bool anyLimit = false;

  std::string token;
  while (in >> token) {
    if (token == "depth") {
      int d = 0;
      in >> d;
      limits.depth = d;
      anyLimit = true;
    } else if (token == "movetime") {
      long long ms = 0;
      in >> ms;
      limits.movetimeMs = uint64_t(ms < 0 ? 0 : ms);
      anyLimit = true;
    } else if (token == "nodes") {
      unsigned long long n = 0;
      in >> n;
      limits.nodes = uint64_t(n);
      anyLimit = true;
    } else if (token == "infinite" || token == "ponder") {
      // No depth or time limit: run until stopped. This has to happen on
      // another thread, otherwise the loop is blocked inside the search and a
      // stop arriving on stdin is never read.
      limits.infinite = true;
      anyLimit = true;
    } else if (token == "perft") {
      // Not supported in this increment; acknowledged so it is not mis-parsed as
      // part of another argument.
      anyLimit = true;
    }
  }

  if (!anyLimit) {
    // "go" with no arguments means "play a normal move". A short default keeps
    // an interactive session responsive.
    limits.depth = 6;
  }

  if (!havePosition_) {
    // Nothing to search. A bestmove is still expected by the protocol.
    out << "info string no position set\n";
    out << "bestmove 0000\n";
    return;
  }

  // Join any previous infinite search before touching the position again.
  finishWorker();

  // Cleared here, before the worker starts, rather than inside the search. If the
  // worker cleared it, the reset could land after a "stop" had already been read
  // from stdin, and that stop would be lost, leaving the search running forever.
  g_searchStopRequested = false;

  if (limits.infinite) {
    worker_ = std::thread([this, limits, &out]() { runSearch(limits, out); });
  } else {
    runSearch(limits, out);
  }
}

bool UciEngine::execute(const std::string& line, std::ostream& out) {
  std::istringstream in(line);
  std::string cmd;
  in >> cmd;

  if (cmd.empty()) return true;

  if (cmd == "uci") {
    out << "id name " << kEngineName << "\n";
    out << "id author PrathamKapoor\n";
    // Option list. The search actually reads Hash; the rest are accepted so a GUI
    // does not complain about options it always sends.
    out << "option name Hash type spin default 16 min 1 max 1024\n";
    out << "option name Threads type spin default 1 min 1 max 1\n";
    out << "option name Ponder type check default false\n";
    out << "uciok\n";
    return true;
  }

  if (cmd == "isready") {
    out << "readyok\n";
    return true;
  }

  if (cmd == "ucinewgame") {
    havePosition_ = false;
    g_searchStopRequested = true;  // stop anything still running from before
    return true;
  }

  if (cmd == "setoption") {
    // "setoption name <id> value <v>". Only Hash matters to this increment.
    std::string name, value;
    std::string part;
    bool inName = false;
    while (in >> part) {
      if (part == "name") { inName = true; continue; }
      if (part == "value") { inName = false; continue; }
      if (inName) name += (name.empty() ? "" : " ") + part;
      else value += (value.empty() ? "" : " ") + part;
    }
    if (name == "Hash") {
      int mb = 16;
      try {
        mb = std::stoi(value);
      } catch (...) {
        mb = 16;
      }
      if (mb < 1) mb = 1;
      if (mb > 1024) mb = 1024;
      options_.ttSizeMB = mb;
    }
    return true;
  }

  if (cmd == "position") {
    finishWorker();
    applyPosition(in);
    return true;
  }

  if (cmd == "go") {
    go(in, out);
    return true;
  }

  if (cmd == "stop") {
    g_searchStopRequested = true;
    return true;
  }

  if (cmd == "ponderhit") return true;

  if (cmd == "quit") {
    // An infinite search must not outlive the process.
    g_searchStopRequested = true;
    finishWorker();
    return false;
  }

  // Unknown commands are ignored, as the protocol requires.
  return true;
}

void UciEngine::loop(std::istream& in, std::ostream& out) {
  std::string line;
  while (std::getline(in, line)) {
    if (!execute(line, out)) break;
    out.flush();
  }
}

}  // namespace pocket