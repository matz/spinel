# A program's own Array#inspect that inspects its elements in turn: p runs
# it for the outer Array and, through it, for each Array inside, as CRuby
# does, without looping (see array_reopen_inspect_p.rb).

class Array
  def inspect = "L(" + map(&:inspect).join(",") + ")"
end

p [1, [2, 3], "a"]
p [[1.5], [], [:x]]
p({k: [1], j: [[2]]})
x = [[1, 2], [3]]
x[1] << 4
p x
puts [5, [6]].inspect
puts [5, [6]].to_s
