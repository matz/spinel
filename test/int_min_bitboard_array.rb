# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# Chess bitboards: one 64-bit word per piece, bit i = square i (a1 = 0 ...
# h8 = 63). The h8 square is the sign bit, -2**63, built here without an
# overflow so every mode agrees with CRuby; the rook sits on a1 so no mask
# with the sign bit is decremented. Usage: KING_SQUARE ROUNDS
WHITE_ROOK, WHITE_KING, BLACK_KING = 0, 1, 2

def bit(sq) = sq < 63 ? 1 << sq : -9223372036854775807 - (sq - 62)
def file_mask(f) = (0..7).sum { |r| bit(r * 8 + f) }
def rank_mask(r) = (0..7).sum { |f| bit(r * 8 + f) }
def rook_attacks(sq) = (file_mask(sq % 8) | rank_mask(sq / 8)) & ~bit(sq)

def popcount(bb)
  n = 0
  while bb != 0
    bb &= bb - 1
    n += 1
  end
  n
end

king_sq = ARGV[0].to_i
rounds  = ARGV[1].to_i

boards = Array.new(3, 0)
boards[WHITE_ROOK] = bit(0)
boards[WHITE_KING] = bit(4)
boards[BLACK_KING] = bit(king_sq)

in_check = nil
attacked = 0
rounds.times do
  rook = rook_attacks(0)
  king = boards[BLACK_KING]
  in_check = king.nil? ? :no_king : (rook & king) != 0
  attacked += popcount(rook)
end
puts "black king on #{king_sq}: in check = #{in_check.inspect}"
puts "black king bitboard: #{boards[BLACK_KING]}"
puts "attacked squares counted: #{attacked}"
p boards
p boards.map(&:nil?)
p boards.count { |bb| bb != 0 }
p boards.min
p boards.index(bit(king_sq))
p boards.sum
p boards.map { |bb| bb.zero? }
p boards.select(&:negative?)
p boards.any? { |bb| bb & bit(63) != 0 }
