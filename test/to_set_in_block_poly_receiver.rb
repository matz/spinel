# A Set built by `.to_set` inside a block, read back out of the Array the block
# builds: the receiver of `size` is a poly value there, and the class facts that
# say which classes it can hold were solved before `to_set` was rewritten to
# Set.new, so they read it as a builtin value and left Set's arms out of the
# dispatch. `size`, `length`, `empty?` and `add` then answered NoMethodError
# (Kilau K-044). `Set.new(a)` in the block was never affected.
require "set"

sets = [[1, 1], [2]].map { |a| a.to_set }
p sets[0].size
p sets[0].length
p sets[1].empty?
p sets[0].add(5).size
p sets.map(&:size)
p sets.map { |s| s.include?(2) }

mixed = [[3, 3, 4], (1..3), { a: 1 }].map { |e| e.to_set }
p mixed.map(&:size)

p [[1, 1], [2]].map { |a| Set.new(a) }.map(&:size)
