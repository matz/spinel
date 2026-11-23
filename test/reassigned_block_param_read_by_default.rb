# A block parameter the body assigns, that a default of the same list reads: the
# default runs before the body, so it sees the argument; and an underscore
# parameter, which says it is unused, is never split from its slot.
p proc { |_x, y = _x| _x = 9; y }.call(7)
p proc { |x, y = x| x = 9; y }.call(7)
p proc { |x, y = (x + 1)| x = 9; [x, y] }.call(7)
p proc { |x, k: x| x = 9; [x, k] }.call(7)
p proc { |x, y = 1| x = x + 1; [x, y] }.call(7)
p [[1, 2], [3, 4]].map { |a, b = a| a = b * 10; [a, b] }
