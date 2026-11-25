# A statement-form concat runs its arguments in order: an argument whose
# code a block or a begin runs ahead of itself runs after the ones before it.
$log = []
def mark(v) = ($log << :mark; v)
def g(s, xs) = s.concat(mark("é"), (xs.map { |e| $log << :blk; e }.join), 256)
p g(+"", ["a", "b"]), $log
$log = []
def h(s, xs)
  s.concat(mark("x"), (xs.map { |e| $log << :blk; e }.join))
  s
end
p h(+"", ["a", "b"]), $log
$log = []
def k(s, xs)
  s.concat(mark("é"), (begin; $log << :beg; xs.join; rescue; "r"; end), 256)
  s
end
p k(+"", ["c"]), $log
$log = []
def m(s, xs)
  s.concat(mark("y"), xs.map { |e| $log << :blk; e.to_s }.join, mark("z"))
  s
end
p m(+"", [1, 2]), $log
