/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <cassert>
#include <cstring>   // For std::memset, std::memcmp
#include <iomanip>
#include <sstream>

#include "bitboard.h"
#include "misc.h"
#include "movegen.h"
#include "position.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"

using std::string;

namespace Zobrist {

  Key psq[PIECE_NB][SQUARE_NB];
  Key enpassant[FILE_NB];
  Key castling[CASTLING_RIGHT_NB];
  Key side;
#ifdef THREECHECK
  Key checks[COLOR_NB][CHECKS_NB];
#endif
}

namespace {

const string PieceToChar(" PNBRQK  pnbrqk");

// min_attacker() is a helper function used by see() to locate the least
// valuable attacker for the side to move, remove the attacker we just found
// from the bitboards and scan for new X-ray attacks behind it.

template<int Pt>
PieceType min_attacker(const Bitboard* bb, Square to, Bitboard stmAttackers,
                       Bitboard& occupied, Bitboard& attackers) {

  Bitboard b = stmAttackers & bb[Pt];
  if (!b)
      return min_attacker<Pt+1>(bb, to, stmAttackers, occupied, attackers);

  occupied ^= b & ~(b - 1);

  if (Pt == PAWN || Pt == BISHOP || Pt == QUEEN)
      attackers |= attacks_bb<BISHOP>(to, occupied) & (bb[BISHOP] | bb[QUEEN]);

  if (Pt == ROOK || Pt == QUEEN)
      attackers |= attacks_bb<ROOK>(to, occupied) & (bb[ROOK] | bb[QUEEN]);

  attackers &= occupied; // After X-ray that may add already processed pieces
  return (PieceType)Pt;
}

template<>
PieceType min_attacker<KING>(const Bitboard*, Square, Bitboard, Bitboard&, Bitboard&) {
  return KING; // No need to update bitboards: it is the last cycle
}

} // namespace


/// operator<<(Position) returns an ASCII representation of the position

std::ostream& operator<<(std::ostream& os, const Position& pos) {

  os << "\n +---+---+---+---+---+---+---+---+\n";

  for (Rank r = RANK_8; r >= RANK_1; --r)
  {
      for (File f = FILE_A; f <= FILE_H; ++f)
          os << " | " << PieceToChar[pos.piece_on(make_square(f, r))];

      os << " |\n +---+---+---+---+---+---+---+---+\n";
  }

  os << "\nFen: " << pos.fen() << "\nKey: " << std::hex << std::uppercase
     << std::setfill('0') << std::setw(16) << pos.key() << std::dec << "\nCheckers: ";

  for (Bitboard b = pos.checkers(); b; )
      os << UCI::square(pop_lsb(&b)) << " ";

  return os;
}


/// Position::init() initializes at startup the various arrays used to compute
/// hash keys.

void Position::init() {

  PRNG rng(1070372);

  for (Color c = WHITE; c <= BLACK; ++c)
      for (PieceType pt = PAWN; pt <= KING; ++pt)
          for (Square s = SQ_A1; s <= SQ_H8; ++s)
              Zobrist::psq[make_piece(c, pt)][s] = rng.rand<Key>();

  for (File f = FILE_A; f <= FILE_H; ++f)
      Zobrist::enpassant[f] = rng.rand<Key>();

  for (int cr = NO_CASTLING; cr <= ANY_CASTLING; ++cr)
  {
      Zobrist::castling[cr] = 0;
      Bitboard b = cr;
      while (b)
      {
          Key k = Zobrist::castling[1ULL << pop_lsb(&b)];
          Zobrist::castling[cr] ^= k ? k : rng.rand<Key>();
      }
  }

  Zobrist::side = rng.rand<Key>();
#ifdef THREECHECK
  for (Color c = WHITE; c <= BLACK; ++c)
      for (Checks n = CHECKS_0; n <= CHECKS_3; ++n)
          Zobrist::checks[c][n] = rng.rand<Key>();
#endif
}


/// Position::set() initializes the position object with the given FEN string.
/// This function is not very robust - make sure that input FENs are correct,
/// this is assumed to be the responsibility of the GUI.

Position& Position::set(const string& fenStr, int v, StateInfo* si, Thread* th) {
/*
   A FEN string defines a particular position using only the ASCII character set.

   A FEN string contains six fields separated by a space. The fields are:

   1) Piece placement (from white's perspective). Each rank is described, starting
      with rank 8 and ending with rank 1. Within each rank, the contents of each
      square are described from file A through file H. Following the Standard
      Algebraic Notation (SAN), each piece is identified by a single letter taken
      from the standard English names. White pieces are designated using upper-case
      letters ("PNBRQK") whilst Black uses lowercase ("pnbrqk"). Blank squares are
      noted using digits 1 through 8 (the number of blank squares), and "/"
      separates ranks.

   2) Active color. "w" means white moves next, "b" means black.

   3) Castling availability. If neither side can castle, this is "-". Otherwise,
      this has one or more letters: "K" (White can castle kingside), "Q" (White
      can castle queenside), "k" (Black can castle kingside), and/or "q" (Black
      can castle queenside).

   4) En passant target square (in algebraic notation). If there's no en passant
      target square, this is "-". If a pawn has just made a 2-square move, this
      is the position "behind" the pawn. This is recorded regardless of whether
      there is a pawn in position to make an en passant capture.

   5) Halfmove clock. This is the number of halfmoves since the last pawn advance
      or capture. This is used to determine if a draw can be claimed under the
      fifty-move rule.

   6) Fullmove number. The number of the full move. It starts at 1, and is
      incremented after Black's move.
*/

  unsigned char col, row, token;
  size_t idx;
  Square sq = SQ_A8;
  std::istringstream ss(fenStr);

  std::memset(this, 0, sizeof(Position));
  std::memset(si, 0, sizeof(StateInfo));
  std::fill_n(&pieceList[0][0], sizeof(pieceList) / sizeof(Square), SQ_NONE);
  st = si;
  var = v;

  ss >> std::noskipws;

  // 1. Piece placement
  while ((ss >> token) && !isspace(token))
  {
      if (isdigit(token))
          sq += Square(token - '0'); // Advance the given number of files

      else if (token == '/')
          sq -= Square(16);

      else if ((idx = PieceToChar.find(token)) != string::npos)
      {
          put_piece(Piece(idx), sq);
          ++sq;
      }
  }

  // 2. Active color
  ss >> token;
  sideToMove = (token == 'w' ? WHITE : BLACK);
  ss >> token;

  // 3. Castling availability. Compatible with 3 standards: Normal FEN standard,
  // Shredder-FEN that uses the letters of the columns on which the rooks began
  // the game instead of KQkq and also X-FEN standard that, in case of Chess960,
  // if an inner rook is associated with the castling right, the castling tag is
  // replaced by the file letter of the involved rook, as for the Shredder-FEN.
  while ((ss >> token) && !isspace(token))
  {
      Square rsq;
      Color c = islower(token) ? BLACK : WHITE;
      Rank rank = relative_rank(c, RANK_1);
      Square ksq = square<KING>(c);
      if (rank_of(ksq) != rank)
          continue;
      Piece rook = make_piece(c, ROOK);

      token = char(toupper(token));

      if (token == 'K')
          for (rsq = relative_square(c, SQ_H1); rsq != ksq && piece_on(rsq) != rook; --rsq) {}

      else if (token == 'Q')
          for (rsq = relative_square(c, SQ_A1); rsq != ksq && piece_on(rsq) != rook; ++rsq) {}

      else if (token >= 'A' && token <= 'H')
          rsq = make_square(File(token - 'A'), rank);

      else
          continue;

      if (rsq != ksq)
          set_castling_right(c, rsq);
  }

  // 4. En passant square. Ignore if no pawn capture is possible
  if (((ss >> col) && (col >= 'a' && col <= 'h'))
        && ((ss >> row) && (sideToMove ? row == '3' : row == '6')))
  {
      st->epSquare = make_square(File(col - 'a'), Rank(row - '1'));

      if (!(attackers_to(st->epSquare) & pieces(sideToMove, PAWN)))
          st->epSquare = SQ_NONE;
      else if (SquareBB[st->epSquare] & pieces())
          st->epSquare = SQ_NONE;
      else if (sideToMove == WHITE && (shift_bb<DELTA_N>(SquareBB[st->epSquare]) & pieces()))
          st->epSquare = SQ_NONE;
      else if (sideToMove == BLACK && (shift_bb<DELTA_S>(SquareBB[st->epSquare]) & pieces()))
          st->epSquare = SQ_NONE;
      else if (sideToMove == WHITE && !(shift_bb<DELTA_S>(SquareBB[st->epSquare]) & pieces(BLACK, PAWN)))
          st->epSquare = SQ_NONE;
      else if (sideToMove == BLACK && !(shift_bb<DELTA_N>(SquareBB[st->epSquare]) & pieces(WHITE, PAWN)))
          st->epSquare = SQ_NONE;
  }
  else
      st->epSquare = SQ_NONE;

  // 5-6. Halfmove clock and fullmove number
  ss >> std::skipws >> st->rule50 >> gamePly;

#ifdef THREECHECK
    st->checksGiven[WHITE] = CHECKS_0;
    st->checksGiven[BLACK] = CHECKS_0;
    if ((v & THREECHECK_VARIANT) != 0)
    {
        // 7. Checks given counter for Three-Check positions
        if ((ss >> std::skipws >> token) && token == '+')
        {
            ss >> token;
            switch(token - '0')
            {
            case 0: st->checksGiven[WHITE] = CHECKS_0; break;
            case 1: st->checksGiven[WHITE] = CHECKS_1; break;
            case 2: st->checksGiven[WHITE] = CHECKS_2; break;
            case 3: st->checksGiven[WHITE] = CHECKS_3; break;
            default: st->checksGiven[WHITE] = CHECKS_NB;
            }
            if ((ss >> token) && token == '+') {
                ss >> token;
                switch(token - '0')
                {
                case 0: st->checksGiven[BLACK] = CHECKS_0; break;
                case 1: st->checksGiven[BLACK] = CHECKS_1; break;
                case 2: st->checksGiven[BLACK] = CHECKS_2; break;
                case 3: st->checksGiven[BLACK] = CHECKS_3; break;
                default : st->checksGiven[BLACK] = CHECKS_NB;
                }
            }
        }
    }
#endif

  // Convert from fullmove starting from 1 to ply starting from 0,
  // handle also common incorrect FEN with fullmove = 0.
  gamePly = std::max(2 * (gamePly - 1), 0) + (sideToMove == BLACK);

  thisThread = th;
  set_state(st);

  assert(pos_is_ok());

  return *this;
}


/// Position::set_castling_right() is a helper function used to set castling
/// rights given the corresponding color and the rook starting square.

void Position::set_castling_right(Color c, Square rfrom) {

  Square kfrom = square<KING>(c);
  CastlingSide cs = kfrom < rfrom ? KING_SIDE : QUEEN_SIDE;
  CastlingRight cr = (c | cs);

  st->castlingRights |= cr;
  castlingRightsMask[kfrom] |= cr;
  castlingRightsMask[rfrom] |= cr;
  castlingRookSquare[cr] = rfrom;

  Square kto = relative_square(c, cs == KING_SIDE ? SQ_G1 : SQ_C1);
  Square rto = relative_square(c, cs == KING_SIDE ? SQ_F1 : SQ_D1);

  for (Square s = std::min(rfrom, rto); s <= std::max(rfrom, rto); ++s)
      if (s != kfrom && s != rfrom)
          castlingPath[cr] |= s;

  for (Square s = std::min(kfrom, kto); s <= std::max(kfrom, kto); ++s)
      if (s != kfrom && s != rfrom)
          castlingPath[cr] |= s;
}


/// Position::set_check_info() sets king attacks to detect if a move gives check

void Position::set_check_info(StateInfo* si) const {

  si->blockersForKing[WHITE] = slider_blockers(pieces(BLACK), square<KING>(WHITE));
  si->blockersForKing[BLACK] = slider_blockers(pieces(WHITE), square<KING>(BLACK));

  Square ksq = square<KING>(~sideToMove);
#ifdef HORDE
  if (is_horde() && ksq == SQ_NONE) {
  si->checkSquares[PAWN]   = 0;
  si->checkSquares[KNIGHT] = 0;
  si->checkSquares[BISHOP] = 0;
  si->checkSquares[ROOK]   = 0;
  si->checkSquares[QUEEN]  = 0;
  si->checkSquares[KING]   = 0;
  return;
  }
#endif
#ifdef ATOMIC
  if (is_atomic() && ksq == SQ_NONE) {
  si->checkSquares[PAWN]   = 0;
  si->checkSquares[KNIGHT] = 0;
  si->checkSquares[BISHOP] = 0;
  si->checkSquares[ROOK]   = 0;
  si->checkSquares[QUEEN]  = 0;
  si->checkSquares[KING]   = 0;
  return;
  }
#endif
#ifdef ANTI
  if (is_anti()) {
  si->checkSquares[PAWN]   = 0;
  si->checkSquares[KNIGHT] = 0;
  si->checkSquares[BISHOP] = 0;
  si->checkSquares[ROOK]   = 0;
  si->checkSquares[QUEEN]  = 0;
  si->checkSquares[KING]   = 0;
  return;
  }
#endif
  si->checkSquares[PAWN]   = attacks_from<PAWN>(ksq, ~sideToMove);
  si->checkSquares[KNIGHT] = attacks_from<KNIGHT>(ksq);
  si->checkSquares[BISHOP] = attacks_from<BISHOP>(ksq);
  si->checkSquares[ROOK]   = attacks_from<ROOK>(ksq);
  si->checkSquares[QUEEN]  = si->checkSquares[BISHOP] | si->checkSquares[ROOK];
  si->checkSquares[KING]   = 0;
}


/// Position::set_state() computes the hash keys of the position, and other
/// data that once computed is updated incrementally as moves are made.
/// The function is only used when a new position is set up, and to verify
/// the correctness of the StateInfo data when running in debug mode.

void Position::set_state(StateInfo* si) const {

  si->key = si->pawnKey = si->materialKey = var;
  si->nonPawnMaterial[WHITE] = si->nonPawnMaterial[BLACK] = VALUE_ZERO;
  si->psq = SCORE_ZERO;
  set_check_info(si);
#ifdef RACE
  if (is_race())
  {
      Rank r = rank_of(square<KING>(sideToMove));
      si->checkersBB = r == RANK_8 ? 0 : Rank8BB & square<KING>(~sideToMove);
  }
  else
#endif
#ifdef HORDE
  if (is_horde() && square<KING>(sideToMove) == SQ_NONE)
      si->checkersBB = 0;
  else
#endif
#ifdef ANTI
  if (is_anti())
      si->checkersBB = 0;
  else
#endif
#ifdef ATOMIC
  if (is_atomic() && (square<KING>(sideToMove) == SQ_NONE ||
         (attacks_from<KING>(square<KING>(sideToMove)) & square<KING>(~sideToMove))))
      si->checkersBB = 0;
  else
#endif
  {
      si->checkersBB = attackers_to(square<KING>(sideToMove)) & pieces(~sideToMove);
  }

  for (Bitboard b = pieces(); b; )
  {
      Square s = pop_lsb(&b);
      Piece pc = piece_on(s);
      si->key ^= Zobrist::psq[pc][s];
#ifdef ANTI
      if (is_anti())
          si->psq += PSQT::psqAnti[pc][s];
      else
#endif
      si->psq += PSQT::psq[pc][s];
  }

  if (si->epSquare != SQ_NONE)
      si->key ^= Zobrist::enpassant[file_of(si->epSquare)];

  if (sideToMove == BLACK)
      si->key ^= Zobrist::side;

  si->key ^= Zobrist::castling[si->castlingRights];

  for (Bitboard b = pieces(PAWN); b; )
  {
      Square s = pop_lsb(&b);
      si->pawnKey ^= Zobrist::psq[piece_on(s)][s];
  }

  for (Color c = WHITE; c <= BLACK; ++c)
      for (PieceType pt = PAWN; pt <= KING; ++pt)
          for (int cnt = 0; cnt < pieceCount[make_piece(c, pt)]; ++cnt)
              si->materialKey ^= Zobrist::psq[make_piece(c, pt)][cnt];

  for (Color c = WHITE; c <= BLACK; ++c)
      for (PieceType pt = KNIGHT; pt <= QUEEN; ++pt)
          si->nonPawnMaterial[c] += pieceCount[make_piece(c, pt)] * PieceValue[MG][pt];

#ifdef THREECHECK
  for (Color c = WHITE; c <= BLACK; ++c)
      for (Checks n = CHECKS_1; n <= si->checksGiven[c]; ++n)
          si->key ^= Zobrist::checks[c][n];
#endif
}


/// Position::fen() returns a FEN representation of the position. In case of
/// Chess960 the Shredder-FEN notation is used. This is mainly a debugging function.

const string Position::fen() const {

  int emptyCnt;
  std::ostringstream ss;

  for (Rank r = RANK_8; r >= RANK_1; --r)
  {
      for (File f = FILE_A; f <= FILE_H; ++f)
      {
          for (emptyCnt = 0; f <= FILE_H && empty(make_square(f, r)); ++f)
              ++emptyCnt;

          if (emptyCnt)
              ss << emptyCnt;

          if (f <= FILE_H)
              ss << PieceToChar[piece_on(make_square(f, r))];
      }

      if (r > RANK_1)
          ss << '/';
  }
#ifdef HOUSE
  if (is_house())
      ss << '/'; // TODO: pieces in hand
#endif

  ss << (sideToMove == WHITE ? " w " : " b ");

  bool chess960 = is_chess960();
  if (can_castle(WHITE_OO))
      ss << (chess960 ? char('A' + file_of(castling_rook_square(WHITE |  KING_SIDE))) : 'K');

  if (can_castle(WHITE_OOO))
      ss << (chess960 ? char('A' + file_of(castling_rook_square(WHITE | QUEEN_SIDE))) : 'Q');

  if (can_castle(BLACK_OO))
      ss << (chess960 ? char('a' + file_of(castling_rook_square(BLACK |  KING_SIDE))) : 'k');

  if (can_castle(BLACK_OOO))
      ss << (chess960 ? char('a' + file_of(castling_rook_square(BLACK | QUEEN_SIDE))) : 'q');

  if (!can_castle(WHITE) && !can_castle(BLACK))
      ss << '-';

  ss << (ep_square() == SQ_NONE ? " - " : " " + UCI::square(ep_square()) + " ")
     << st->rule50 << " " << 1 + (gamePly - (sideToMove == BLACK)) / 2;

#ifdef THREECHECK
  if (is_three_check())
      ss << " +" << st->checksGiven[WHITE] << "+" << st->checksGiven[BLACK];
#endif

  return ss.str();
}


/// Position::game_phase() calculates the game phase interpolating total non-pawn
/// material between endgame and midgame limits.

Phase Position::game_phase() const {

  Value npm = st->nonPawnMaterial[WHITE] + st->nonPawnMaterial[BLACK];
#ifdef HORDE
  if (is_horde())
      npm = st->nonPawnMaterial[BLACK] + st->nonPawnMaterial[BLACK];
#endif
#ifdef ATOMIC
  if (is_atomic())
      npm += npm;
#endif

  npm = std::max(EndgameLimit, std::min(npm, MidgameLimit));

  return Phase(((npm - EndgameLimit) * PHASE_MIDGAME) / (MidgameLimit - EndgameLimit));
}


/// Position::slider_blockers() returns a bitboard of all the pieces (both colors) that
/// are blocking attacks on the square 's' from 'sliders'. A piece blocks a slider
/// if removing that piece from the board would result in a position where square 's'
/// is attacked. For example, a king-attack blocking piece can be either a pinned or
/// a discovered check piece, according if its color is the opposite or the same of
/// the color of the slider.

Bitboard Position::slider_blockers(Bitboard sliders, Square s) const {

  Bitboard b, pinners, result = 0;
#ifdef HORDE
  if (is_horde() && s == SQ_NONE) return result;
#endif
#ifdef ANTI
  if (is_anti() && s == SQ_NONE) return result;
#endif

  // Pinners are sliders that attack 's' when a pinned piece is removed
  pinners = (  (PseudoAttacks[ROOK  ][s] & pieces(QUEEN, ROOK))
             | (PseudoAttacks[BISHOP][s] & pieces(QUEEN, BISHOP))) & sliders;

  while (pinners)
  {
      b = between_bb(s, pop_lsb(&pinners)) & pieces();

      if (!more_than_one(b))
          result |= b;
  }
  return result;
}


/// Position::attackers_to() computes a bitboard of all pieces which attack a
/// given square. Slider attacks use the occupied bitboard to indicate occupancy.

Bitboard Position::attackers_to(Square s, Bitboard occupied) const {

  return  (attacks_from<PAWN>(s, BLACK)    & pieces(WHITE, PAWN))
        | (attacks_from<PAWN>(s, WHITE)    & pieces(BLACK, PAWN))
        | (attacks_from<KNIGHT>(s)         & pieces(KNIGHT))
        | (attacks_bb<ROOK  >(s, occupied) & pieces(ROOK,   QUEEN))
        | (attacks_bb<BISHOP>(s, occupied) & pieces(BISHOP, QUEEN))
        | (attacks_from<KING>(s)           & pieces(KING));
}


/// Position::legal() tests whether a pseudo-legal move is legal

bool Position::legal(Move m) const {

  assert(is_ok(m));

  Color us = sideToMove;
  Square from = from_sq(m);

  assert(color_of(moved_piece(m)) == us);
#ifdef ANTI
  // If a player can capture, that player must capture
  // Is handled by move generator
  assert(!is_anti() || capture(m) == can_capture());
  if (is_anti())
      return true;
#endif
#ifdef HORDE
  assert(is_horde() && us == WHITE ? square<KING>(us) == SQ_NONE : piece_on(square<KING>(us)) == make_piece(us, KING));
#else
  assert(piece_on(square<KING>(us)) == make_piece(us, KING));
#endif

#ifdef RACE
  // Checking moves are illegal
  if (is_race() && gives_check(m))
      return false;
#endif
#ifdef HORDE
  // All pseudo-legal moves by the horde are legal
  if (is_horde() && square<KING>(us) == SQ_NONE)
      return true;
#endif
#ifdef ATOMIC
  if (is_atomic())
  {
      Square ksq = square<KING>(us);
      Square to = to_sq(m);
      if (capture(m) && (attacks_from<KING>(to) & ksq))
          return false;
      if (type_of(piece_on(from)) != KING)
      {
          if (attacks_from<KING>(square<KING>(~us)) & ksq)
              return true;
          if (capture(m))
          {
              Square capsq = type_of(m) == ENPASSANT ? make_square(file_of(to), rank_of(from)) : to;
              Bitboard blast = attacks_from<KING>(to) & (pieces() ^ pieces(PAWN));
              if (blast & square<KING>(~us))
                  return true;
              Bitboard b = pieces() ^ ((blast | capsq) | from);
              if (checkers() & b)
                  return false;
              if ((attacks_bb<  ROOK>(ksq, b) & pieces(~us, QUEEN, ROOK) & b) ||
                  (attacks_bb<BISHOP>(ksq, b) & pieces(~us, QUEEN, BISHOP) & b))
                  return false;
              return true;
          }
      }
      else if (attacks_from<KING>(square<KING>(~us)) & to)
          return true;
  }
#endif

  // En passant captures are a tricky special case. Because they are rather
  // uncommon, we do it simply by testing whether the king is attacked after
  // the move is made.
  if (type_of(m) == ENPASSANT)
  {
      Square ksq = square<KING>(us);
      Square to = to_sq(m);
      Square capsq = to - pawn_push(us);
      Bitboard occupied = (pieces() ^ from ^ capsq) | to;

      assert(to == ep_square());
      assert(moved_piece(m) == make_piece(us, PAWN));
      assert(piece_on(capsq) == make_piece(~us, PAWN));
      assert(piece_on(to) == NO_PIECE);

      return   !(attacks_bb<  ROOK>(ksq, occupied) & pieces(~us, QUEEN, ROOK))
            && !(attacks_bb<BISHOP>(ksq, occupied) & pieces(~us, QUEEN, BISHOP));
  }

#ifdef ATOMIC
  if (is_atomic() && type_of(piece_on(from)) == KING && type_of(m) != CASTLING)
  {
      Square ksq = square<KING>(~us);
      Square to = to_sq(m);
      if ((attacks_from<KING>(ksq) & from) && !(attacks_from<KING>(ksq) & to))
      {
          if (attackers_to(to) & pieces(~us, KNIGHT, PAWN))
              return false;
          Bitboard occupied = (pieces() ^ from) | to;
          return   !(attacks_bb<  ROOK>(to, occupied) & pieces(~us, QUEEN, ROOK))
                && !(attacks_bb<BISHOP>(to, occupied) & pieces(~us, QUEEN, BISHOP));
      }
  }
#endif
  // If the moving piece is a king, check whether the destination
  // square is attacked by the opponent. Castling moves are checked
  // for legality during move generation.
  if (type_of(piece_on(from)) == KING)
      return type_of(m) == CASTLING || !(attackers_to(to_sq(m)) & pieces(~us));

  // A non-king move is legal if and only if it is not pinned or it
  // is moving along the ray towards or away from the king.
  return   !(pinned_pieces(us) & from)
        ||  aligned(from, to_sq(m), square<KING>(us));
}


/// Position::pseudo_legal() takes a random move and tests whether the move is
/// pseudo legal. It is used to validate moves from TT that can be corrupted
/// due to SMP concurrent access or hash position key aliasing.

bool Position::pseudo_legal(const Move m) const {

  Color us = sideToMove;
  Square from = from_sq(m);
  Square to = to_sq(m);
  Piece pc = moved_piece(m);

#ifdef KOTH
  // If the game is already won or lost, further moves are illegal
  if (is_koth() && (is_koth_win() || is_koth_loss()))
      return false;
#endif
#ifdef RACE
  // If the game is already won or lost, further moves are illegal
  if (is_race() && (is_race_draw() || is_race_win() || is_race_loss()))
      return false;
#endif
#ifdef HORDE
  // If the game is already won or lost, further moves are illegal
  if (is_horde() && is_horde_loss())
      return false;
#endif
#ifdef ANTI
  // If the game is already won or lost, further moves are illegal
  if (is_anti() && (is_anti_win() || is_anti_loss()))
      return false;
#endif
#ifdef ATOMIC
  if (is_atomic())
  {
      // If the game is already won or lost, further moves are illegal
      if (is_atomic_win() || is_atomic_loss())
          return false;
      if (pc == NO_PIECE || color_of(pc) != us)
          return false;
      if (capture(m))
      {
          if (type_of(pc) == KING)
              return false;
          Square ksq = square<KING>(us);
          if ((pieces(us) & to) || (attacks_from<KING>(ksq) & to))
              return false;
          if (!(attacks_from<KING>(square<KING>(~us)) & ksq))
          {
              // Illegal pawn capture generated by killer move heuristic
              if (type_of(pc) == PAWN && file_of(from) == file_of(to))
                 return false;
              Square capsq = type_of(m) == ENPASSANT ? make_square(file_of(to), rank_of(from)) : to;
              Bitboard blast = attacks_from<KING>(to) & (pieces() ^ pieces(PAWN));
              if (blast & square<KING>(~us))
                  return true;
              Bitboard b = pieces() ^ ((blast | capsq) | from);
              if (checkers() & b)
                  return false;
              if ((attacks_bb<  ROOK>(ksq, b) & pieces(~us, QUEEN, ROOK) & b) ||
                  (attacks_bb<BISHOP>(ksq, b) & pieces(~us, QUEEN, BISHOP) & b))
                  return false;
          }
      }
  }
#endif

  // Use a slower but simpler function for uncommon cases
  if (type_of(m) != NORMAL)
      return MoveList<LEGAL>(*this).contains(m);

  // Is not a promotion, so promotion piece must be empty
  if (promotion_type(m) - KNIGHT != NO_PIECE_TYPE)
      return false;

  // If the 'from' square is not occupied by a piece belonging to the side to
  // move, the move is obviously not legal.
  if (pc == NO_PIECE || color_of(pc) != us)
      return false;

  // The destination square cannot be occupied by a friendly piece
  if (pieces(us) & to)
      return false;

  // Handle the special case of a pawn move
  if (type_of(pc) == PAWN)
  {
      // We have already handled promotion moves, so destination
      // cannot be on the 8th/1st rank.
      if (rank_of(to) == relative_rank(us, RANK_8))
          return false;

      if (   !(attacks_from<PAWN>(from, us) & pieces(~us) & to) // Not a capture
          && !((from + pawn_push(us) == to) && empty(to))       // Not a single push
          && !(   (from + 2 * pawn_push(us) == to)              // Not a double push
               && (rank_of(from) == relative_rank(us, RANK_2))
               && empty(to)
               && empty(to - pawn_push(us))))
          return false;
  }
  else if (!(attacks_from(pc, from) & to))
      return false;

  // Evasions generator already takes care to avoid some kind of illegal moves
  // and legal() relies on this. We therefore have to take care that the same
  // kind of moves are filtered out here.
#ifdef ATOMIC
  if (is_atomic() && (attacks_from<KING>(square<KING>(~us)) & (type_of(pc) == KING ? to : square<KING>(us))))
      return true;
#endif
  if (checkers())
  {
      if (type_of(pc) != KING)
      {
          // Double check? In this case a king move is required
          if (more_than_one(checkers()))
              return false;

          // Our move must be a blocking evasion or a capture of the checking piece
          if (!((between_bb(lsb(checkers()), square<KING>(us)) | checkers()) & to))
              return false;
      }
      // In case of king moves under check we have to remove king so as to catch
      // invalid moves like b1a1 when opposite queen is on c1.
      else if (attackers_to(to, pieces() ^ from) & pieces(~us))
          return false;
  }

  return true;
}


/// Position::gives_check() tests whether a pseudo-legal move gives a check

bool Position::gives_check(Move m) const {

  assert(is_ok(m));
  assert(color_of(moved_piece(m)) == sideToMove);

  Square from = from_sq(m);
  Square to = to_sq(m);

#ifdef HORDE
  if (is_horde() && square<KING>(~sideToMove) == SQ_NONE)
      return false;
#endif
#ifdef ANTI
  if (is_anti())
      return false;
#endif
#ifdef ATOMIC
  if (is_atomic())
  {
      Square ksq = square<KING>(~sideToMove);
      if (is_horde() && ksq == SQ_NONE)
          return false;
      // If kings are adjacent, there is no check
      // If kings were adjacent, there may be direct checks
      if (type_of(piece_on(from)) == KING)
      {
          if (attacks_from<KING>(ksq) & to)
              return false;
          else if (attacks_from<KING>(ksq) & from)
          {
              if (attackers_to(ksq) & pieces(sideToMove, KNIGHT, PAWN))
                  return true;
              Bitboard occupied = (pieces() ^ from) | to;
              return   (attacks_bb<  ROOK>(ksq, occupied) & pieces(sideToMove, QUEEN, ROOK))
                    || (attacks_bb<BISHOP>(ksq, occupied) & pieces(sideToMove, QUEEN, BISHOP));
          }
      }
      else if (attacks_from<KING>(ksq) & square<KING>(sideToMove))
          return false;
      if (capture(m))
      {
          // Do blasted pieces discover checks?
          Square capsq = type_of(m) == ENPASSANT ? make_square(file_of(to), rank_of(from)) : to;
          Bitboard blast = attacks_from<KING>(to) & (pieces() ^ pieces(PAWN));
          if (blast & ksq) // Variant ending
              return false;
          Bitboard b = pieces() ^ ((blast | capsq) | from);

          return (attacks_bb<  ROOK>(ksq, b) & pieces(sideToMove, QUEEN, ROOK) & b)
              || (attacks_bb<BISHOP>(ksq, b) & pieces(sideToMove, QUEEN, BISHOP) & b);
      }
  }
#endif

  // Is there a direct check?
  if (st->checkSquares[type_of(piece_on(from))] & to)
      return true;

  // Is there a discovered check?
  if (   (discovered_check_candidates() & from)
      && !aligned(from, to, square<KING>(~sideToMove)))
      return true;

  switch (type_of(m))
  {
  case NORMAL:
      return false;

  case PROMOTION:
      return attacks_bb(Piece(promotion_type(m)), to, pieces() ^ from) & square<KING>(~sideToMove);

  // En passant capture with check? We have already handled the case
  // of direct checks and ordinary discovered check, so the only case we
  // need to handle is the unusual case of a discovered check through
  // the captured pawn.
  case ENPASSANT:
  {
      Square capsq = make_square(file_of(to), rank_of(from));
      Bitboard b = (pieces() ^ from ^ capsq) | to;

      return  (attacks_bb<  ROOK>(square<KING>(~sideToMove), b) & pieces(sideToMove, QUEEN, ROOK))
            | (attacks_bb<BISHOP>(square<KING>(~sideToMove), b) & pieces(sideToMove, QUEEN, BISHOP));
  }
  case CASTLING:
  {
      Square kfrom = from;
      Square rfrom = to; // Castling is encoded as 'King captures the rook'
      Square kto = relative_square(sideToMove, rfrom > kfrom ? SQ_G1 : SQ_C1);
      Square rto = relative_square(sideToMove, rfrom > kfrom ? SQ_F1 : SQ_D1);

      return   (PseudoAttacks[ROOK][rto] & square<KING>(~sideToMove))
            && (attacks_bb<ROOK>(rto, (pieces() ^ kfrom ^ rfrom) | rto | kto) & square<KING>(~sideToMove));
  }
  default:
      assert(false);
      return false;
  }
}

/// Position::do_move() makes a move, and saves all information necessary
/// to a StateInfo object. The move is assumed to be legal. Pseudo-legal
/// moves should be filtered out before this function is called.

void Position::do_move(Move m, StateInfo& newSt, bool givesCheck) {

  assert(is_ok(m));
  assert(&newSt != st);

  ++nodes;
  Key k = st->key ^ Zobrist::side;

  // Copy some fields of the old state to our new StateInfo object except the
  // ones which are going to be recalculated from scratch anyway and then switch
  // our state pointer to point to the new (ready to be updated) state.
  std::memcpy(&newSt, st, offsetof(StateInfo, key));
  newSt.previous = st;
  st = &newSt;

  // Increment ply counters. In particular, rule50 will be reset to zero later on
  // in case of a capture or a pawn move.
  ++gamePly;
  ++st->rule50;
  ++st->pliesFromNull;

  Color us = sideToMove;
  Color them = ~us;
  Square from = from_sq(m);
  Square to = to_sq(m);
  Piece pc = piece_on(from);
  Piece captured = type_of(m) == ENPASSANT ? make_piece(them, PAWN) : piece_on(to);

  assert(color_of(pc) == us);
  assert(captured == NO_PIECE || color_of(captured) == (type_of(m) != CASTLING ? them : us));
  assert(type_of(captured) != KING);
#ifdef ANTI
  assert(is_anti() || type_of(captured) != KING);
#else
  assert(type_of(captured) != KING);
#endif

  if (type_of(m) == CASTLING)
  {
      assert(pc == make_piece(us, KING));
      assert(captured == make_piece(us, ROOK));

      Square rfrom, rto;
      do_castling<true>(us, from, to, rfrom, rto);

      st->psq += PSQT::psq[captured][rto] - PSQT::psq[captured][rfrom];
      k ^= Zobrist::psq[captured][rfrom] ^ Zobrist::psq[captured][rto];
      captured = NO_PIECE;
  }

  if (captured)
  {
      Square capsq = to;

      // If the captured piece is a pawn, update pawn hash key, otherwise
      // update non-pawn material.
      if (type_of(captured) == PAWN)
      {
          if (type_of(m) == ENPASSANT)
          {
              capsq -= pawn_push(us);

              assert(pc == make_piece(us, PAWN));
              assert(to == st->epSquare);
              assert(relative_rank(us, to) == RANK_6);
              assert(piece_on(to) == NO_PIECE);
              assert(piece_on(capsq) == make_piece(them, PAWN));

              board[capsq] = NO_PIECE; // Not done by remove_piece()
          }

          st->pawnKey ^= Zobrist::psq[captured][capsq];
      }
      else
          st->nonPawnMaterial[them] -= PieceValue[MG][captured];

      // Update board and piece lists
      remove_piece(captured, capsq);

      // Update material hash key and prefetch access to materialTable
      k ^= Zobrist::psq[captured][capsq];
      st->materialKey ^= Zobrist::psq[captured][pieceCount[captured]];
#ifdef ATOMIC
      if (is_atomic()) // Remove the blast piece(s)
      {
          Bitboard blast = attacks_from<KING>(to);
          while (blast)
          {
              Square bsq = pop_lsb(&blast);
              if (bsq == from)
                  continue;
              st->blast[bsq] = piece_on(bsq);
              Piece bpc = st->blast[bsq];
              if (bpc != NO_PIECE && type_of(bpc) != PAWN)
              {
                  Color bc = color_of(st->blast[bsq]);
                  st->nonPawnMaterial[bc] -= PieceValue[MG][type_of(bpc)];

                  // Update board and piece lists
                  remove_piece(bpc, bsq);

                  // Update material hash key
                  k ^= Zobrist::psq[bpc][bsq];
                  st->materialKey ^= Zobrist::psq[bpc][pieceCount[bpc]];

                  // Update incremental scores
                  st->psq -= PSQT::psq[bpc][bsq];

                  // Update castling rights if needed
                  if (st->castlingRights && castlingRightsMask[bsq])
                  {
                      int cr = castlingRightsMask[bsq];
                      k ^= Zobrist::castling[st->castlingRights & cr];
                      st->castlingRights &= ~cr;
                  }
              }
          }
      }
#endif

      prefetch(thisThread->materialTable[st->materialKey]);

      // Update incremental scores
#ifdef ANTI
      if (is_anti())
          st->psq -= PSQT::psqAnti[captured][capsq];
      else
#endif
      st->psq -= PSQT::psq[captured][capsq];

      // Reset rule 50 counter
      st->rule50 = 0;
  }

#ifdef ATOMIC
  if (is_atomic() && captured)
      k ^= Zobrist::psq[pc][from];
  else
#endif
  // Update hash key
  k ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];

  // Reset en passant square
  if (st->epSquare != SQ_NONE)
  {
      k ^= Zobrist::enpassant[file_of(st->epSquare)];
      st->epSquare = SQ_NONE;
  }

  // Update castling rights if needed
  if (st->castlingRights && (castlingRightsMask[from] | castlingRightsMask[to]))
  {
      int cr = castlingRightsMask[from] | castlingRightsMask[to];
      k ^= Zobrist::castling[st->castlingRights & cr];
      st->castlingRights &= ~cr;
  }

#ifdef THREECHECK
  if (is_three_check() && givesCheck)
  {
      ++(st->checksGiven[sideToMove]);
      Checks checksGiven = checks_given();
      assert(checksGiven < CHECKS_NB);
      k ^= Zobrist::checks[sideToMove][checksGiven];
  }
#endif

#ifdef ATOMIC
  if (is_atomic() && captured) // Remove the blast piece(s)
  {
      st->blast[from] = piece_on(from);
      remove_piece(pc, from);
      // Update material (hash key already updated)
      st->materialKey ^= Zobrist::psq[pc][pieceCount[pc]];
      if (type_of(pc) != PAWN)
          st->nonPawnMaterial[us] -= PieceValue[MG][type_of(pc)];
  }
  else
#endif
  // Move the piece. The tricky Chess960 castling is handled earlier
  if (type_of(m) != CASTLING)
      move_piece(pc, from, to);

  // If the moving piece is a pawn do some special extra work
  if (type_of(pc) == PAWN)
  {
      // Set en-passant square if the moved pawn can be captured
#ifdef HORDE
      if (is_horde() && rank_of(from) == relative_rank(us, RANK_1)); else
#endif
      if (   (int(to) ^ int(from)) == 16
          && (attacks_from<PAWN>(to - pawn_push(us), us) & pieces(them, PAWN)))
      {
          st->epSquare = (from + to) / 2;
          k ^= Zobrist::enpassant[file_of(st->epSquare)];
      }
#ifdef ATOMIC
      else if (is_atomic() && captured)
      {
      }
#endif
      else if (type_of(m) == PROMOTION)
      {
          Piece promotion = make_piece(us, promotion_type(m));

          assert(relative_rank(us, to) == RANK_8);
#ifdef ANTI
          assert(type_of(promotion) >= KNIGHT && type_of(promotion) <= (is_anti() ? KING : QUEEN));
#else
          assert(type_of(promotion) >= KNIGHT && type_of(promotion) <= QUEEN);
#endif

          remove_piece(pc, to);
          put_piece(promotion, to);

          // Update hash keys
          k ^= Zobrist::psq[pc][to] ^ Zobrist::psq[promotion][to];
          st->pawnKey ^= Zobrist::psq[pc][to];
          st->materialKey ^=  Zobrist::psq[promotion][pieceCount[promotion]-1]
                            ^ Zobrist::psq[pc][pieceCount[pc]];

          // Update incremental score
#ifdef ANTI
          if (is_anti())
              st->psq += PSQT::psqAnti[promotion][to] - PSQT::psqAnti[pc][to];
          else
#endif
          st->psq += PSQT::psq[promotion][to] - PSQT::psq[pc][to];

          // Update material
          st->nonPawnMaterial[us] += PieceValue[MG][promotion];
      }

#ifdef ATOMIC
      if (is_atomic() && captured)
          st->pawnKey ^= Zobrist::psq[make_piece(us, PAWN)][from];
      else
#endif
      // Update pawn hash key and prefetch access to pawnsTable
      st->pawnKey ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];
      prefetch(thisThread->pawnsTable[st->pawnKey]);

      // Reset rule 50 draw counter
      st->rule50 = 0;
  }

#ifdef ATOMIC
  if (is_atomic() && captured)
      st->psq -= PSQT::psq[pc][from];
  else
#endif
  // Update incremental scores
#ifdef ANTI
  if (is_anti())
      st->psq += PSQT::psqAnti[pc][to] - PSQT::psqAnti[pc][from];
  else
#endif
  st->psq += PSQT::psq[pc][to] - PSQT::psq[pc][from];

  // Set capture piece
  st->capturedPiece = captured;

  // Update the key with the final value
  st->key = k;

#ifdef ATOMIC
  if (is_atomic() && captured && is_atomic_win())
      givesCheck = false;
#endif
#ifdef RACE
  if (is_race())
      st->checkersBB = Rank8BB & square<KING>(us);
  else
#endif
#ifdef ANTI
  if (is_anti())
      st->checkersBB = 0;
  else
#endif
  // Calculate checkers bitboard (if move gives check)
  st->checkersBB = givesCheck ? attackers_to(square<KING>(them)) & pieces(us) : 0;

  sideToMove = ~sideToMove;

  // Update king attacks used for fast check detection
  set_check_info(st);

  assert(pos_is_ok());
}


/// Position::undo_move() unmakes a move. When it returns, the position should
/// be restored to exactly the same state as before the move was made.

void Position::undo_move(Move m) {

  assert(is_ok(m));

  sideToMove = ~sideToMove;

  Color us = sideToMove;
  Square from = from_sq(m);
  Square to = to_sq(m);
  Piece pc = piece_on(to);
#ifdef ATOMIC
  if (is_atomic() && st->capturedPiece) // Restore the blast piece(s)
      pc = st->blast[from];
#endif

  assert(empty(to) || color_of(piece_on(to)) == us);
  assert(empty(from) || type_of(m) == CASTLING);
#ifdef ANTI
  assert(is_anti() || type_of(st->capturedPiece) != KING);
#else
  assert(type_of(st->capturedPiece) != KING);
#endif

  if (type_of(m) == PROMOTION)
  {
      assert(relative_rank(us, to) == RANK_8);
#ifdef ATOMIC
      if (!is_atomic() || !st->capturedPiece)
      {
#endif
      assert(type_of(pc) == promotion_type(m));
#ifdef ANTI
      assert(type_of(pc) >= KNIGHT && type_of(pc) <= (is_anti() ? KING : QUEEN));
#else
      assert(type_of(pc) >= KNIGHT && type_of(pc) <= QUEEN);
#endif

      remove_piece(pc, to);
      pc = make_piece(us, PAWN);
      put_piece(pc, to);
#ifdef ATOMIC
      }
#endif
  }

  if (type_of(m) == CASTLING)
  {
      Square rfrom, rto;
      do_castling<false>(us, from, to, rfrom, rto);
  }
  else
  {
#ifdef ATOMIC
      if (is_atomic() && st->capturedPiece) // Restore the blast piece(s)
          put_piece(pc, from);
      else
#endif
      move_piece(pc, to, from); // Put the piece back at the source square

      if (st->capturedPiece)
      {
          Square capsq = to;

          if (type_of(m) == ENPASSANT)
          {
              capsq -= pawn_push(us);

              assert(type_of(pc) == PAWN);
              assert(to == st->previous->epSquare);
              assert(relative_rank(us, to) == RANK_6);
              assert(piece_on(capsq) == NO_PIECE);
              assert(st->capturedPiece == make_piece(~us, PAWN));
          }

#ifdef ATOMIC
          if (is_atomic() && st->capturedPiece) // Restore the blast piece(s)
          {
              Bitboard blast = attacks_from<KING>(to); // squares in blast radius
              while (blast)
              {
                  Square bsq = pop_lsb(&blast);
                  if (bsq == from)
                      continue;
                  Piece bpc = st->blast[bsq];
                  if (bpc != NO_PIECE && type_of(bpc) != PAWN)
                      put_piece(bpc, bsq);
              }
          }
#endif
          put_piece(st->capturedPiece, capsq); // Restore the captured piece
      }
  }

  // Finally point our state pointer back to the previous state
  st = st->previous;
  --gamePly;

  assert(pos_is_ok());
}


/// Position::do_castling() is a helper used to do/undo a castling move. This
/// is a bit tricky in Chess960 where from/to squares can overlap.
template<bool Do>
void Position::do_castling(Color us, Square from, Square& to, Square& rfrom, Square& rto) {

  bool kingSide = to > from;
  rfrom = to; // Castling is encoded as "king captures friendly rook"
  rto = relative_square(us, kingSide ? SQ_F1 : SQ_D1);
  to = relative_square(us, kingSide ? SQ_G1 : SQ_C1);

  // Remove both pieces first since squares could overlap in Chess960
  remove_piece(make_piece(us, KING), Do ? from : to);
  remove_piece(make_piece(us, ROOK), Do ? rfrom : rto);
  board[Do ? from : to] = board[Do ? rfrom : rto] = NO_PIECE; // Since remove_piece doesn't do it for us
  put_piece(make_piece(us, KING), Do ? to : from);
  put_piece(make_piece(us, ROOK), Do ? rto : rfrom);
}


/// Position::do(undo)_null_move() is used to do(undo) a "null move": It flips
/// the side to move without executing any move on the board.

void Position::do_null_move(StateInfo& newSt) {

  assert(!checkers());
  assert(&newSt != st);

  std::memcpy(&newSt, st, sizeof(StateInfo));
  newSt.previous = st;
  st = &newSt;

  if (st->epSquare != SQ_NONE)
  {
      st->key ^= Zobrist::enpassant[file_of(st->epSquare)];
      st->epSquare = SQ_NONE;
  }

  st->key ^= Zobrist::side;
  prefetch(TT.first_entry(st->key));

  ++st->rule50;
  st->pliesFromNull = 0;

  sideToMove = ~sideToMove;

  set_check_info(st);

  assert(pos_is_ok());
}

void Position::undo_null_move() {

  assert(!checkers());

  st = st->previous;
  sideToMove = ~sideToMove;
}


/// Position::key_after() computes the new hash key after the given move. Needed
/// for speculative prefetch. It doesn't recognize special moves like castling,
/// en-passant and promotions.

Key Position::key_after(Move m) const {

  Square from = from_sq(m);
  Square to = to_sq(m);
  Piece pc = piece_on(from);
  Piece captured = piece_on(to);
  Key k = st->key ^ Zobrist::side;

  if (captured)
  {
      k ^= Zobrist::psq[captured][to];
#ifdef ATOMIC
      if (is_atomic())
      {
          Bitboard blast = (attacks_from<KING>(to) & (pieces() ^ pieces(PAWN))) - from;
          while (blast)
          {
              Square bsq = pop_lsb(&blast);
              Piece bpc = st->blast[bsq];
              k ^= Zobrist::psq[bpc][bsq];
          }
      }
#endif
  }

  return k ^ Zobrist::psq[pc][to] ^ Zobrist::psq[pc][from];
}


/// Position::see() is a static exchange evaluator: It tries to estimate the
/// material gain or loss resulting from a move.

Value Position::see_sign(Move m) const {

  assert(is_ok(m));

#ifdef THREECHECK
  if (is_three_check() && gives_check(m))
      return VALUE_KNOWN_WIN;
#endif
  // Early return if SEE cannot be negative because captured piece value
  // is not less then capturing one. Note that king moves always return
  // here because king midgame value is set to 0.
  if (PieceValue[MG][moved_piece(m)] <= PieceValue[MG][piece_on(to_sq(m))])
      return VALUE_KNOWN_WIN;

  return see(m);
}

Value Position::see(Move m) const {

  Square from, to;
  Bitboard occupied, attackers, stmAttackers;
#ifdef HORDE
  Value swapList[SQUARE_NB];
#else
  Value swapList[32];
#endif
  int slIndex = 1;
  PieceType captured;
  Color stm;

  assert(is_ok(m));

  from = from_sq(m);
  to = to_sq(m);
  swapList[0] = PieceValue[MG][piece_on(to)];
  stm = color_of(piece_on(from));
  occupied = pieces() ^ from;
#ifdef ATOMIC
  if (is_atomic())
  {
    Value blast_eval = VALUE_ZERO;
    Bitboard blast = attacks_from<KING>(to) & (pieces() ^ pieces(PAWN)) & ~SquareBB[from];
    if (blast & pieces(~stm,KING))
        return VALUE_MATE;
    for (Color c = WHITE; c <= BLACK; ++c)
        for (PieceType pt = KNIGHT; pt <= QUEEN; ++pt)
            if (c == stm)
                blast_eval -= popcount(blast & pieces(c,pt))*PieceValue[MG][pt];
            else
                blast_eval += popcount(blast & pieces(c,pt))*PieceValue[MG][pt];
    return blast_eval + PieceValue[MG][piece_on(to_sq(m))] - PieceValue[MG][moved_piece(m)];
  }
#endif

  // Castling moves are implemented as king capturing the rook so cannot
  // be handled correctly. Simply return VALUE_ZERO that is always correct
  // unless in the rare case the rook ends up under attack.
  if (type_of(m) == CASTLING)
      return VALUE_ZERO;

  if (type_of(m) == ENPASSANT)
  {
      occupied ^= to - pawn_push(stm); // Remove the captured pawn
      swapList[0] = PieceValue[MG][PAWN];
  }

  // Find all attackers to the destination square, with the moving piece
  // removed, but possibly an X-ray attacker added behind it.
  attackers = attackers_to(to, occupied) & occupied;

  // If the opponent has no attackers we are finished
  stm = ~stm;
  stmAttackers = attackers & pieces(stm);
  if (!stmAttackers)
      return swapList[0];

  // The destination square is defended, which makes things rather more
  // difficult to compute. We proceed by building up a "swap list" containing
  // the material gain or loss at each stop in a sequence of captures to the
  // destination square, where the sides alternately capture, and always
  // capture with the least valuable piece. After each capture, we look for
  // new X-ray attacks from behind the capturing piece.
  captured = type_of(piece_on(from));

  do {
#ifdef HORDE
      assert(slIndex < SQUARE_NB);
#else
      assert(slIndex < 32);
#endif

      // Add the new entry to the swap list
      swapList[slIndex] = -swapList[slIndex - 1] + PieceValue[MG][captured];

      // Locate and remove the next least valuable attacker
      captured = min_attacker<PAWN>(byTypeBB, to, stmAttackers, occupied, attackers);
      stm = ~stm;
      stmAttackers = attackers & pieces(stm);
      ++slIndex;

  } while (stmAttackers && (captured != KING || (--slIndex, false))); // Stop before a king capture

  // Having built the swap list, we negamax through it to find the best
  // achievable score from the point of view of the side to move.
  while (--slIndex)
      swapList[slIndex - 1] = std::min(-swapList[slIndex], swapList[slIndex - 1]);

  return swapList[0];
}


/// Position::is_draw() tests whether the position is drawn by 50-move rule
/// or by repetition. It does not detect stalemates.

bool Position::is_draw() const {

  if (st->rule50 > 99 && (!checkers() || MoveList<LEGAL>(*this).size()))
      return true;

  StateInfo* stp = st;
  for (int i = 2, rep = 1, e = std::min(st->rule50, st->pliesFromNull); i <= e; i += 2)
  {
      stp = stp->previous->previous;

      if (stp->key == st->key && (++rep >= 2 + (gamePly - i < thisThread->rootPos.game_ply())))
          return true; // Draw at first repetition in search, and second repetition in game tree.
  }

  return false;
}


/// Position::flip() flips position with the white and black sides reversed. This
/// is only useful for debugging e.g. for finding evaluation symmetry bugs.

void Position::flip() {

  string f, token;
  std::stringstream ss(fen());

  for (Rank r = RANK_8; r >= RANK_1; --r) // Piece placement
  {
      std::getline(ss, token, r > RANK_1 ? '/' : ' ');
      f.insert(0, token + (f.empty() ? " " : "/"));
  }

  ss >> token; // Active color
  f += (token == "w" ? "B " : "W "); // Will be lowercased later

  ss >> token; // Castling availability
  f += token + " ";

  std::transform(f.begin(), f.end(), f.begin(),
                 [](char c) { return char(islower(c) ? toupper(c) : tolower(c)); });

  ss >> token; // En passant square
  f += (token == "-" ? token : token.replace(1, 1, token[1] == '3' ? "6" : "3"));

  std::getline(ss, token); // Half and full moves
  f += token;

  set(f, var, st, this_thread());

  assert(pos_is_ok());
}


/// Position::pos_is_ok() performs some consistency checks for the position object.
/// This is meant to be helpful when debugging.

bool Position::pos_is_ok(int* failedStep) const {

  const bool Fast = true; // Quick (default) or full check?

  enum { Default, King, Bitboards, State, Lists, Castling };

  for (int step = Default; step <= (Fast ? Default : Castling); step++)
  {
      if (failedStep)
          *failedStep = step;

      if (step == Default)
      {
          Square wksq = square<KING>(WHITE), bksq = square<KING>(BLACK);
#ifdef ANTI
          if (is_anti())
          {
              if ((sideToMove != WHITE && sideToMove != BLACK)
                  || (ep_square() != SQ_NONE && relative_rank(sideToMove, ep_square()) != RANK_6))
                  return false;
          }
          else
#endif
          if (   (sideToMove != WHITE && sideToMove != BLACK)
#ifdef HORDE
#ifdef ATOMIC
              || (is_horde() ? wksq != SQ_NONE : ((!is_atomic() || wksq != SQ_NONE) && piece_on(wksq) != W_KING))
#else
              || (is_horde() ? wksq != SQ_NONE : piece_on(wksq) != W_KING)
#endif
#else
#ifdef ATOMIC
              || ((!is_atomic() || wksq != SQ_NONE) && piece_on(wksq) != W_KING)
#else
              || piece_on(wksq) != W_KING
#endif
#endif
#ifdef ATOMIC
              || ((!is_atomic() || bksq != SQ_NONE) && piece_on(bksq) != B_KING)
#else
              || piece_on(bksq) != B_KING
#endif
              || (   ep_square() != SQ_NONE
#ifdef HORDE
                  && (!is_horde() || relative_rank(sideToMove, ep_square()) != RANK_7)
#endif
                  && relative_rank(sideToMove, ep_square()) != RANK_6))
              return false;
      }

      if (step == King)
      {
#ifdef ANTI
          if (is_anti()) {} else
#endif
#ifdef HORDE
          if (is_horde())
          {
              if (   std::count(board, board + SQUARE_NB, W_KING) != 0
                  || std::count(board, board + SQUARE_NB, B_KING) != 1
                  || (sideToMove == WHITE && attackers_to(square<KING>(~sideToMove)) & pieces(sideToMove)))
              return false;
          } else
#endif
#ifdef ATOMIC
          if (is_atomic() && (is_atomic_win() || is_atomic_loss()))
          {
              if (std::count(board, board + SQUARE_NB, W_KING) +
                  std::count(board, board + SQUARE_NB, B_KING) != 1)
              return false;
          }
          else if (is_atomic() && (attacks_from<KING>(square<KING>(~sideToMove)) & square<KING>(sideToMove)))
          {
          } else
#endif
          if (   std::count(board, board + SQUARE_NB, W_KING) != 1
              || std::count(board, board + SQUARE_NB, B_KING) != 1
              || attackers_to(square<KING>(~sideToMove)) & pieces(sideToMove))
              return false;
      }

      if (step == Bitboards)
      {
          if (  (pieces(WHITE) & pieces(BLACK))
              ||(pieces(WHITE) | pieces(BLACK)) != pieces())
              return false;

          for (PieceType p1 = PAWN; p1 <= KING; ++p1)
              for (PieceType p2 = PAWN; p2 <= KING; ++p2)
                  if (p1 != p2 && (pieces(p1) & pieces(p2)))
                      return false;
      }

      if (step == State)
      {
          StateInfo si = *st;
          set_state(&si);
          if (std::memcmp(&si, st, sizeof(StateInfo)))
              return false;
      }

      if (step == Lists)
          for (Color c = WHITE; c <= BLACK; ++c)
              for (PieceType pt = PAWN; pt <= KING; ++pt)
              {
                  Piece pc = make_piece(c, pt);

                  if (pieceCount[pc] != popcount(pieces(c, pt)))
                      return false;

                  for (int i = 0; i < pieceCount[pc]; ++i)
                      if (board[pieceList[pc][i]] != pc || index[pieceList[pc][i]] != i)
                          return false;
              }

      if (step == Castling)
          for (Color c = WHITE; c <= BLACK; ++c)
              for (CastlingSide s = KING_SIDE; s <= QUEEN_SIDE; s = CastlingSide(s + 1))
              {
                  if (!can_castle(c | s))
                      continue;

                  if (   piece_on(castlingRookSquare[c | s]) != make_piece(c, ROOK)
                      || castlingRightsMask[castlingRookSquare[c | s]] != (c | s)
                      ||(castlingRightsMask[square<KING>(c)] & (c | s)) != (c | s))
                      return false;
              }
  }

  return true;
}
