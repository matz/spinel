# Array#flat_map where the block returns a poly_array failed to type
# the result; the inferred receiver-array type clashed with the
# generated sp_PolyArray * inner accumulator.

a = [1, 2].flat_map { |x| [x, x.to_s] }
puts a.length
