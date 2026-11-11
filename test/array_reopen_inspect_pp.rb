# CRuby's pp lays an Array out by its own pretty_print, so the program's
# own Array#inspect does not run for it, nested in a Hash or another Array
# either; p still runs it
class Array; def inspect = "ARR"; end
pp [1]
pp [[1], 2]
x = pp([3])
p x
pp({a: [1]})
pp 1, [2]
a = [5]
pp a, *[[6]]
def m(v) = pp(v)
q = m([7])
p q
h = {k: [8]}
pp h
p h
p [1]
# the program's own #inspect that pp calls runs as outside a pp: its p and
# its inspect calls run the program's Array#inspect, and after a raise out
# of it p still does
class Q
  def initialize = @a = [1]
  def inspect
    p @a
    pp @a
    "Q"
  end
end
pp Q.new
$n = 0
class Boom
  def inspect
    $n += 1
    raise "boom" if $n == 1
    "Boom"
  end
end
begin
  pp 1, Boom.new
rescue => e
  puts e.message
end
p [1]
pp [2]
S = Struct.new(:a)
pp [S.new([1])]
# pp prints the rest of its value without the program's Array#inspect after
# it calls the program's own #inspect or #to_s of an element
class Foo
  def inspect = "FOO"
end
S2 = Struct.new(:a, :b)
pp S2.new(Foo.new, [1])
pp [S2.new(Foo.new, 2), [3]]
pp [Foo.new, [5]]
class E < StandardError
  def to_s = "e"
end
pp [E.new, [1]]
pp [[2], E.new, [3]]
p [1]
