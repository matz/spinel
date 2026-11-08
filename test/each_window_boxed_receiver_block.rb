# spinel: gc-stress
# each_slice / each_cons on a parameter one caller passes an Integer Array
# and another an Array of untyped elements: the boxed receiver hands its
# windows out as Arrays of boxed values, and the block reads them so
# (#9933).
def store(ids)
  ids.each_slice(2) do |chunk|
    values = chunk.map { |id| id }
    p values
  end
end
store([1, 2, 3])
store([[4], [5]].map { |r| r[0] })

def rows(ids, html)
  out = []
  ids.each_slice(2) { |chunk| out << chunk.flat_map { |id| [id, html] } }
  out
end
p rows([1, 2, 3], "x")
p rows([[4], ["y"]].map { |r| r[0] }, "z")

def pairs(xs)
  r = []
  xs.each_cons(2) { |w| r << w.sum }
  xs.each_slice(2) { |a, b| r << [a, b] }
  r
end
p pairs([1, 2, 3])
p pairs([[1.5], [2]].map { |r| r[0] })
