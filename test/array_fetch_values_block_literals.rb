p [1, 2].fetch_values(3) { |index| [index] }
log = []
p [10, 20].fetch_values(0, 4, -3) { |index| log << index; [index] }
p log
p ["a"].fetch_values(2) { |index| { index: index } }
# Each missing index must evaluate the block afresh, while present ones skip it.
n = 0
p [1].fetch_values(5, 0, 6) { |index| n += 1; [index, n] }
p n
