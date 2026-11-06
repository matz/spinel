# A block whose parameter reads the boxed side, called with a Bignum past the
# 32-bit Integer word: the sp_int slot filled beside it is dead, so filling it
# must not raise. On wasm32 it raised "value out of range for 32-bit Integer".
# spinel: wasm
# spinel: wasm-int-overflow: promote
require "set"
seen = { 9_007_199_254_740_000 => true, 5 => true }
[Set[9_007_199_254_740_000], [5], { 7 => 1 }, [4_611_686_018_427_387_904], [-1_099_511_627_776, 1_099_511_627_776]].each do |ids|
  p ids.any? { |id| seen.key?(id) }
  p ids.map { |id| id }
end
big = 1_099_511_627_776 + 3
p [big, 1].select { |x| x.is_a?(Integer) && x > 2 }
p [big].sum { |x| x }
