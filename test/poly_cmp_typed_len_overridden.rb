# spinel: gc-stress
# A program that gives Array a size of its own, or puts a module with one
# ahead of Hash, runs that method in a boxed comparison's argument: it is
# not a count read in place, and the receiver is read before it runs.

class Array
  def size
    $hits += 1
    $x = 100
    length
  end
end
module Counted
  def size
    $hits += 10
    $y = -1
    super
  end
end
class Hash
  prepend Counted
end

$hits = 0
$x = [nil, 1].last
p($x < [1, 2, 3].size, $x)
$y = [nil, 7].last
p($y > { a: 1 }.size, $y)
p $hits
