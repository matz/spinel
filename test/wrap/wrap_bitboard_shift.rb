# spinel: not-cruby -- --int-overflow=wrap: bit 63 built by shifting is
# -2**63 here (CRuby: 2**63, a Bignum).
# Chess bitboards: one 64-bit word per piece, bit i = square i (a1 = 0 ...
# h8 = 63), built by repeated shifting. The h8 square is the sign bit.
WHITE_ROOK, WHITE_KING, BLACK_KING = 0, 1, 2

def bit(sq)
  b = 1
  sq.times { b <<= 1 }
  b
end
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
boards[WHITE_ROOK] = bit(7)
boards[WHITE_KING] = bit(4)
boards[BLACK_KING] = bit(king_sq)

in_check = nil
attacked = 0
rounds.times do
  rook = rook_attacks(7)
  king = boards[BLACK_KING]
  in_check = king.nil? ? :no_king : (rook & king) != 0
  attacked += popcount(rook)
end
puts "black king on #{king_sq}: in check = #{in_check.inspect}"
puts "black king bitboard: #{boards[BLACK_KING]}"
puts "attacked squares counted: #{attacked}"
puts "rook attacks: #{rook_attacks(7)}"
puts "h-file: #{file_mask(7)}"
