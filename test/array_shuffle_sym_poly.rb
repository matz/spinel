# Array#shuffle / Array#shuffle! used to skip sym_array and poly_array.
# For sym_array the dispatcher silently fell through (bin output empty);
# for poly_array the runtime had no shuffle helper at all.

# sym_array
sa = [:a, :b, :c, :d].shuffle
puts sa.length

# poly_array
pa = [1, "a", :s, 2.0].shuffle
puts pa.length
