#include "position.hpp"

#include <cstring>
#include <sstream>

namespace pocket {

namespace {

uint64_t kZobristPsq[16][kNumSquares];
uint64_t kZobristCastling[16];
uint64_t kZobristEpFile[8];
uint64_t kZobristSide;
bool g_zobristReady = false;

// splitmix64: deterministic, so keys are reproducible across runs and machines
// without shipping a .rand file.
uint64_t splitmix64(uint64_t& state) {
  state += 0x9E3779B97F4A7C15ULL;
  uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

}  // namespace

void initZobrist() {
  if (g_zobristReady) return;
  g_zobristReady = true;
  uint64_t rng = 0x5DEECE66DULL;
  for (int p = 0; p < 16; ++p)
    for (int s = 0; s < kNumSquares; ++s) kZobristPsq[p][s] = splitmix64(rng);
  for (int i = 0; i < 16; ++i) kZobristCastling[i] = splitmix64(rng);
  for (int i = 0; i < 8; ++i) kZobristEpFile[i] = splitmix64(rng);
  kZobristSide = splitmix64(rng);
}

namespace {
// Touched from clear() so any Position is usable without an explicit init call.
const bool kForceZobrist = [] { initZobrist(); return true; }();
}  // namespace

// --- Piece placement --------------------------------------------------------

void Position::putPiece(Piece pc, Square s) {
  const Bitboard b = squareBB(s);
  board_[int(s)] = pc;
  byTypeBB_[int(typeOf(pc))] |= b;
  byColorBB_[int(colorOf(pc))] |= b;
  state_->key ^= kZobristPsq[int(pc)][int(s)];
  if (typeOf(pc) == kPawn) state_->pawnKey ^= kZobristPsq[int(pc)][int(s)];
  if (typeOf(pc) != kPawn && typeOf(pc) != kKing)
    state_->nonPawnMaterial += pieceValue(typeOf(pc));
}

void Position::removePiece(Square s) {
  const Piece pc = board_[int(s)];
  assert(pc != kNoPiece);
  const Bitboard b = ~squareBB(s);
  board_[int(s)] = kNoPiece;
  byTypeBB_[int(typeOf(pc))] &= b;
  byColorBB_[int(colorOf(pc))] &= b;
  state_->key ^= kZobristPsq[int(pc)][int(s)];
  if (typeOf(pc) == kPawn) state_->pawnKey ^= kZobristPsq[int(pc)][int(s)];
  if (typeOf(pc) != kPawn && typeOf(pc) != kKing)
    state_->nonPawnMaterial -= pieceValue(typeOf(pc));
}

void Position::movePiece(Square from, Square to) {
  const Piece pc = board_[int(from)];
  assert(pc != kNoPiece);
  assert(board_[int(to)] == kNoPiece);
  const Bitboard toggle = squareBB(from) | squareBB(to);
  board_[int(from)] = kNoPiece;
  board_[int(to)] = pc;
  byTypeBB_[int(typeOf(pc))] ^= toggle;
  byColorBB_[int(colorOf(pc))] ^= toggle;
  state_->key ^= kZobristPsq[int(pc)][int(from)] ^ kZobristPsq[int(pc)][int(to)];
  if (typeOf(pc) == kPawn)
    state_->pawnKey ^= kZobristPsq[int(pc)][int(from)] ^ kZobristPsq[int(pc)][int(to)];
}

void Position::disallowCastling(Square s) {
  const int mask = castlingRightsMask_[int(s)];
  if (mask & state_->castlingRights) {
    state_->key ^= kZobristCastling[state_->castlingRights];
    state_->castlingRights &= ~mask;
    state_->key ^= kZobristCastling[state_->castlingRights];
  }
}

void Position::clear() {
  (void)kForceZobrist;
  initBitboards();
  std::memset(byTypeBB_, 0, sizeof(byTypeBB_));
  std::memset(byColorBB_, 0, sizeof(byColorBB_));
  std::memset(castlingRightsMask_, 0, sizeof(castlingRightsMask_));
  for (int i = 0; i < kNumSquares; ++i) board_[i] = kNoPiece;

  sideToMove_ = kWhite;
  gamePly_ = 0;
  checkers_ = 0;
  state_ = stateStack_;
  *state_ = StateInfo();
}

Bitboard Position::attackersTo(Square s, Bitboard occ) const {
  // Sliders must be split by move type. Intersecting rook attacks with all of
  // (B,R,Q) would count a bishop sitting on a rank/file as an attacker, and
  // likewise for a rook on a diagonal.
  return (pawnAttacksFrom(s, kWhite) & pieces(kBlack, kPawn)) |
         (pawnAttacksFrom(s, kBlack) & pieces(kWhite, kPawn)) |
         (kKnightAttacksBB[s] & pieces(kKnight)) | (kKingAttacksBB[s] & pieces(kKing)) |
         (bishopAttacks(s, occ) & pieces(kBishop, kQueen)) |
         (rookAttacks(s, occ) & pieces(kRook, kQueen));
}

void Position::setCheckers() {
  checkers_ = attackersTo(kingSquare(sideToMove_), pieces()) & pieces(~sideToMove_);
}

// --- FEN --------------------------------------------------------------------

bool Position::setFromFen(const std::string& fen, std::string* err) {
  clear();
  std::istringstream ss(fen);
  std::string board, stm, castling, ep, half, full;
  if (!(ss >> board >> stm >> castling >> ep >> half >> full)) {
    if (err) *err = "fen: expected 6 fields, got: " + fen;
    return false;
  }

  Rank rank = kRank8;
  File file = kFileA;
  for (char c : board) {
    if (c == '/') {
      // A rank must be completely filled before the separator.
      if (file != kFileNone) {
        if (err) *err = "fen: rank does not end at the h-file";
        return false;
      }
      if (rank == kRank1) {
        if (err) *err = "fen: too many ranks";
        return false;
      }
      rank = Rank(int(rank) - 1);
      file = kFileA;
      continue;
    }

    if (c >= '1' && c <= '8') {
      file = File(int(file) + (c - '0'));
      if (file > kFileNone) {
        if (err) *err = "fen: run of empty squares overflows the h-file";
        return false;
      }
      continue;
    }

    Piece pc = kNoPiece;
    switch (c) {
      case 'P': pc = kWhitePawn; break;
      case 'N': pc = kWhiteKnight; break;
      case 'B': pc = kWhiteBishop; break;
      case 'R': pc = kWhiteRook; break;
      case 'Q': pc = kWhiteQueen; break;
      case 'K': pc = kWhiteKing; break;
      case 'p': pc = kBlackPawn; break;
      case 'n': pc = kBlackKnight; break;
      case 'b': pc = kBlackBishop; break;
      case 'r': pc = kBlackRook; break;
      case 'q': pc = kBlackQueen; break;
      case 'k': pc = kBlackKing; break;
      default:
        if (err) *err = std::string("fen: bad piece char '") + c + "'";
        return false;
    }
    if (file > kFileH) {
      if (err) *err = "fen: piece placed past the h-file";
      return false;
    }
    putPiece(pc, makeSquare(file, rank));
    file = File(int(file) + 1);
  }
  if (rank != kRank1 || file != kFileNone) {
    if (err) *err = "fen: board must end on rank 1 with the h-file filled";
    return false;
  }

  // Exactly one king per side is required for the position to be meaningful.
  if (popcount(pieces(kWhite, kKing)) != 1 || popcount(pieces(kBlack, kKing)) != 1) {
    if (err) *err = "fen: each side must have exactly one king";
    return false;
  }

  if (stm == "w") sideToMove_ = kWhite;
  else if (stm == "b") sideToMove_ = kBlack;
  else {
    if (err) *err = "fen: side to move must be w or b";
    return false;
  }

  state_->castlingRights = 0;
  for (char c : castling) {
    CastlingRight cr;
    switch (c) {
      case 'K': cr = kWhiteKingSide; break;
      case 'Q': cr = kWhiteQueenSide; break;
      case 'k': cr = kBlackKingSide; break;
      case 'q': cr = kBlackQueenSide; break;
      default: continue;  // '-' or already-seen duplicate
    }
    // The right is only granted if the king and rook are actually in place.
    const Color col = Color(int(cr) >= 4 ? kBlack : kWhite);
    const bool kingSide = (cr == kWhiteKingSide || cr == kBlackKingSide);
    const Square rsq = castlingRookSquare(col, kingSide);
    if (pieceOn(rsq) == makePiece(col, kRook) && kingSquare(col) == makeSquare(kFileE, Rank(int(col) * 7)))
      state_->castlingRights |= int(cr);
  }

  state_->key ^= kZobristCastling[state_->castlingRights];

  // Squaring the king invalidates both of its side's rights, so king squares
  // carry a mask covering both of that side's rooks' rights.
  for (Color c : {kWhite, kBlack})
    castlingRightsMask_[int(kingSquare(c))] =
        int(castlingRight(c, true)) | int(castlingRight(c, false));
  const Square wr1 = castlingRookSquare(kWhite, true), wr2 = castlingRookSquare(kWhite, false);
  const Square br1 = castlingRookSquare(kBlack, true), br2 = castlingRookSquare(kBlack, false);
  castlingRightsMask_[int(wr1)] |= int(kWhiteKingSide);
  castlingRightsMask_[int(wr2)] |= int(kWhiteQueenSide);
  castlingRightsMask_[int(br1)] |= int(kBlackKingSide);
  castlingRightsMask_[int(br2)] |= int(kBlackQueenSide);

  if (ep == "-" || ep.size() < 2) {
    state_->epSquare = kSQNone;
  } else {
    state_->epSquare = makeSquare(File(ep[0] - 'a'), Rank(ep[1] - '1'));
    // Keep the ep square only if the side to move has a pawn that can actually
    // capture onto it. Such a capturer stands on a square that a pawn of the
    // *opposite* colour on the ep square would attack.
    if (!(pawnAttacksFrom(state_->epSquare, ~sideToMove_) &
          pieces(sideToMove_, kPawn)))
      state_->epSquare = kSQNone;
  }
  if (state_->epSquare != kSQNone) state_->key ^= kZobristEpFile[int(fileOf(state_->epSquare))];

  state_->rule50 = half.empty() ? 0 : std::stoi(half);
  gamePly_ = full.empty() ? 0 : (2 * std::stoi(full) - 2 + (sideToMove_ == kBlack ? 1 : 0));
  state_->pliesFromNull = 0;

  if (sideToMove_ == kBlack) state_->key ^= kZobristSide;

  setCheckers();
  return true;
}

std::string Position::toFenString() const {
  std::ostringstream ss;
  for (Rank r = kRank8; r >= kRank1; r = Rank(int(r) - 1)) {
    int run = 0;
    for (File f = kFileA; f <= kFileH; f = File(int(f) + 1)) {
      const Piece pc = pieceOn(makeSquare(f, r));
      if (pc == kNoPiece) {
        ++run;
        continue;
      }
      if (run) {
        ss << run;
        run = 0;
      }
      switch (int(pc)) {
        case kWhitePawn: ss << 'P'; break;
        case kWhiteKnight: ss << 'N'; break;
        case kWhiteBishop: ss << 'B'; break;
        case kWhiteRook: ss << 'R'; break;
        case kWhiteQueen: ss << 'Q'; break;
        case kWhiteKing: ss << 'K'; break;
        case kBlackPawn: ss << 'p'; break;
        case kBlackKnight: ss << 'n'; break;
        case kBlackBishop: ss << 'b'; break;
        case kBlackRook: ss << 'r'; break;
        case kBlackQueen: ss << 'q'; break;
        case kBlackKing: ss << 'k'; break;
      }
    }
    if (run) ss << run;
    if (r != kRank1) ss << '/';
  }

  ss << (sideToMove_ == kWhite ? " w " : " b ");
  if (state_->castlingRights == 0) {
    ss << '-';
  } else {
    if (canCastle(kWhiteKingSide)) ss << 'K';
    if (canCastle(kWhiteQueenSide)) ss << 'Q';
    if (canCastle(kBlackKingSide)) ss << 'k';
    if (canCastle(kBlackQueenSide)) ss << 'q';
  }
  ss << ' ';
  if (state_->epSquare == kSQNone) {
    ss << '-';
  } else {
    ss << char('a' + int(fileOf(state_->epSquare)));
    ss << char('1' + int(rankOf(state_->epSquare)));
  }
  ss << ' ' << state_->rule50 << ' ' << (1 + (gamePly_ - (sideToMove_ == kBlack ? 1 : 0)) / 2);
  return ss.str();
}

std::string Position::fen() const { return toFenString(); }

// --- do / undo --------------------------------------------------------------

void Position::doCastling(Color us, Square kfrom, Square kto, Square rfrom, Square rto) {
  removePiece(kfrom);
  removePiece(rfrom);
  putPiece(makePiece(us, kKing), kto);
  putPiece(makePiece(us, kRook), rto);
}

void Position::doMove(Move m) {
  assert(state_ + 1 < stateStack_ + 1024);
  StateInfo* prev = state_;
  const Color us = sideToMove_;
  const Color them = ~us;
  const Square from = fromSq(m);
  const Square to = toSq(m);
  const Piece pc = board_[int(from)];
  assert(pc != kNoPiece && colorOf(pc) == us);
  const PieceType pt = typeOf(pc);
  const bool isEp = isEnPassant(m);
  const bool isPromo = isPromotion(m);
  const bool isCastle = isCastling(m);

  *++state_ = *prev;
  state_->move = m;
  ++gamePly_;

  // Clear the ep square; it is re-set below only for a double pawn push.
  if (state_->epSquare != kSQNone) {
    state_->key ^= kZobristEpFile[int(fileOf(state_->epSquare))];
    state_->epSquare = kSQNone;
  }

  ++state_->pliesFromNull;

  // Removal of the captured piece.
  if (isEp) {
    const Square capsq = makeSquare(fileOf(to), rankOf(from));
    assert(pieceOn(capsq) == makePiece(them, kPawn));
    state_->key ^= kZobristPsq[int(pieceOn(capsq))][int(capsq)];
    state_->pawnKey ^= kZobristPsq[int(pieceOn(capsq))][int(capsq)];
    state_->nonPawnMaterial -= pieceValue(kPawn);
    state_->capturedPiece = pieceOn(capsq);
    removePiece(capsq);
  } else if (board_[int(to)] != kNoPiece) {
    state_->capturedPiece = board_[int(to)];
    state_->nonPawnMaterial -= pieceValue(typeOf(state_->capturedPiece));
    removePiece(to);
  } else {
    state_->capturedPiece = kNoPiece;
  }

  if (isCastling(m)) {
    const bool kingSide = to > from;
    state_->capturedPiece = kNoPiece;
    doCastling(us, from, to, castlingRookSquare(us, kingSide),
               castlingRookTo(us, kingSide));
  } else {
    movePiece(from, to);
    if (isPromo) {
      removePiece(to);
      putPiece(makePiece(us, promotionType(m)), to);
    }
  }

  disallowCastling(from);
  disallowCastling(to);
  // Moving the king forfeits only that side's rights. disallowCastling(from)
  // already covers both of them via castlingRightsMask_, so clearing the whole
  // mask here would also destroy the opponent's rights.
  // A captured rook on its home square also loses the right.
  if (isCastle == false && (state_->capturedPiece != kNoPiece) &&
      typeOf(state_->capturedPiece) == kRook)
    disallowCastling(to);

  if (pt == kPawn || isCastle || state_->capturedPiece != kNoPiece) state_->rule50 = 0;
  else ++state_->rule50;

  // Double pawn push. Both ends matter: checking only the destination would
  // treat a single push from rank 3 to rank 4 as a double push and set an ep
  // square whose captured pawn does not exist.
  if (pt == kPawn && int(relativeRank(us, from)) == 1 &&
      int(relativeRank(us, to)) == 3) {
    const Square epsq = makeSquare(fileOf(from), Rank((int(rankOf(from)) + int(rankOf(to))) / 2));
    // Record an ep square only if the opponent actually has a pawn able to use
    // it. Such a capturer stands on a square that a pawn of *our* colour on
    // epsq would attack, which is the mirror of our own capture pattern.
    if (pawnAttacksFrom(epsq, us) & pieces(them, kPawn)) {
      state_->epSquare = epsq;
      state_->key ^= kZobristEpFile[int(fileOf(epsq))];
    }
  }

  sideToMove_ = them;
  state_->key ^= kZobristSide;
  setCheckers();
}

void Position::undoMove(Move m) {
  (void)m;
  sideToMove_ = ~sideToMove_;
  const Square from = fromSq(m);
  const Square to = toSq(m);
  const bool isEp = isEnPassant(m);
  const bool isPromo = isPromotion(m);
  const bool isCastle = isCastling(m);

  if (isCastle) {
    const Color us = sideToMove_;
    const bool kingSide = to > from;
    removePiece(to);
    removePiece(castlingRookTo(us, kingSide));
    putPiece(makePiece(us, kKing), from);
    putPiece(makePiece(us, kRook), castlingRookSquare(us, kingSide));
  } else {
    if (isPromo) {
      removePiece(to);
      putPiece(makePiece(sideToMove_, kPawn), to);
    }
    movePiece(to, from);
    // The captured piece, including an en passant pawn, is restored once below
    // from state_->capturedPiece. Restoring it here too would XOR the Zobrist
    // key twice and double-count material.
  }

  if (state_->capturedPiece != kNoPiece) {
    const Square capsq = isEp ? makeSquare(fileOf(to), rankOf(from)) : to;
    putPiece(state_->capturedPiece, capsq);
  }

  --state_;
  setCheckers();
  --gamePly_;
}

void Position::doNullMove() {
  assert(state_ + 1 < stateStack_ + 1024);
  StateInfo* prev = state_;
  *++state_ = *prev;
  ++gamePly_;
  if (state_->epSquare != kSQNone) {
    state_->key ^= kZobristEpFile[int(fileOf(state_->epSquare))];
    state_->epSquare = kSQNone;
  }
  ++state_->rule50;
  state_->pliesFromNull = 0;
  state_->capturedPiece = kNoPiece;
  sideToMove_ = ~sideToMove_;
  state_->key ^= kZobristSide;
  setCheckers();
}

void Position::undoNullMove() {
  sideToMove_ = ~sideToMove_;
  --state_;
  setCheckers();
  --gamePly_;
}

// --- Legality ---------------------------------------------------------------

bool Position::captureOrPromotion(Move m) const {
  return (board_[int(toSq(m))] != kNoPiece) || isEnPassant(m) || isPromotion(m);
}

bool Position::legal(Move m) {
  assert(isOk(fromSq(m)) && isOk(toSq(m)));

  const Color us = sideToMove_;
  const Color them = ~us;
  const Square ksq = kingSquare(us);
  const Square from = fromSq(m);
  const Square to = toSq(m);

  if (isCastling(m)) {
    const bool kingSide = to > from;
    if (!canCastle(castlingRight(us, kingSide))) return false;

    const Square rfrom = castlingRookSquare(us, kingSide);
    if (pieceOn(rfrom) != makePiece(us, kRook)) return false;
    if (inCheck()) return false;  // may not castle out of or through check

    const Square kto = castlingKingTo(us, kingSide);

    // Every square strictly between the king and the rook must be empty. This
    // covers f1/g1 on the king side and b1/c1/d1 on the queen side.
    Bitboard needEmpty = 0;
    const int step = kingSide ? 1 : -1;
    for (Square s = Square(int(from) + step); s != rfrom; s = Square(int(s) + step))
      needEmpty |= squareBB(s);
    if (pieces() & needEmpty) return false;

    // The king itself only walks from its origin to the destination, so on the
    // king side that is f1 and g1 and on the queen side d1 and c1. Squares like
    // e1's other neighbours are never traversed and need not be safe.
    Bitboard kingPath = 0;
    for (Square s = kto; s != from; s = Square(int(s) - step)) kingPath |= squareBB(s);

    // The rook is excluded from the occupancy test: it is our own piece and
    // cannot attack us, but it does block sliders along the rank.
    const Bitboard occ = pieces() ^ rfrom;
    while (kingPath) {
      const Square s = popLsb(kingPath);
      if (attackersTo(s, occ) & pieces(them)) return false;
    }
    return true;
  }

  if (typeOf(pieceOn(from)) == kKing) {
    // The king may not step onto a square attacked by the enemy. The origin is
    // vacated and whatever stood on the destination is captured.
    const Bitboard occ = pieces() ^ from ^ to;
    return (attackersTo(to, occ) & pieces(them)) == 0;
  }

  if (isEnPassant(m)) {
    // Two pawns leave the board on the same rank, which can uncover a slider.
    // Play the move, then check the king of the side that moved: after doMove
    // the side to move is the opponent, so asking inCheck() alone would test
    // the wrong king.
    doMove(m);
    const bool ok = (attackersTo(kingSquare(us), pieces()) & pieces(them)) == 0;
    undoMove(m);
    return ok;
  }

  // Occupancy once the move is played: `from` is vacated, `to` is occupied.
  const Bitboard occ = (pieces() ^ from) | to;
  // The captured piece is no longer an enemy piece, so exclude its square.
  const Bitboard enemies = pieces(them) & ~squareBB(to);
  return (attackersTo(ksq, occ) & enemies) == 0;
}

bool Position::givesCheck(Move m) {
  // Promotion piece type and castling both change which lines are open, so
  // this is answered by actually playing the move. That is cheap enough at the
  // rate it is needed, and removes a whole class of subtle bugs.
  doMove(m);
  const bool check = inCheck();
  undoMove(m);
  return check;
}

int Position::repetitionCount() const {
  const int end = std::min(gamePly_, state_->rule50);
  int count = 0;
  for (int i = 4; i <= end; i += 2) {
    if (stateStack_[gamePly_ - i].key == state_->key) ++count;
  }
  return count;
}

bool Position::hasNonPawnMaterial(Color c) const {
  return (byColorBB_[int(c)] & (byTypeBB_[int(kKnight)] | byTypeBB_[int(kBishop)] |
                                byTypeBB_[int(kRook)] | byTypeBB_[int(kQueen)])) != 0;
}

}  // namespace pocket
