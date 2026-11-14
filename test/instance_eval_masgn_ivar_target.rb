# A multiple assignment's ivar target in a block that instance_eval or
# instance_exec runs is the receiver's, as a plain `@x = v` is: the receiver's
# class (each class a boxed receiver can be) lays it out and takes the value.
class A; def initialize = (@a = 1); end
class B; def initialize = (@b = 2); end

# a receiver of either class, the ivar new to both
p([A.new, B.new].map { |o| o.instance_exec { @fresh, l = 7, 8; [@fresh, l] } })
p([A.new, B.new].map { |o| o.instance_exec { @s, @t = "x", :y; [@s, @t] } })

# one class: an ivar it has, and one it has not
t = A.new
t.instance_eval { @a, @h = 3, 4 }
p t.instance_variables
p t.instance_eval { [@a, @h] }

# in a method of another class, and with a splat
class Host
  def self.fill(o) = o.instance_eval { @a, x = 5, 6; [@a, x] }
  def fill_all(o) = o.instance_eval { @p, *@q = 1, 2, 3; [@p, @q] }
end
p Host.fill(A.new)
p Host.new.fill_all(B.new)

# controls: the class's own ivars, in a class method and at the top level
class Pair
  @n = 1
  @m = 2
  def self.swap = (@n, @m = @m, @n; [@n, @m])
end
p Pair.swap
@top, @top2 = 1, 2
p [@top, @top2]

# targets after the splat store as the ones before it do: into the receiver's
# nil-capable slot, one class or either of two
class PA; def initialize = (@a = 0); def c = @c; end
class PH; def initialize = (@c = :h); def f(o) = o.instance_eval { @a, *, @c = 1, 2, 3 }; end
o = PA.new
PH.new.f(o)
p o.c
class PB; def initialize = (@c = 5); def c = @c; end
xs = [PA.new, PB.new]
xs.each { |q| q.instance_eval { @a, *, @c = 1, 2, 3 } }
p xs.map(&:c)
p [PA.new, PB.new].map { |q| q.instance_exec { *@a, @c = 1, 2, 3; [@a, @c] } }

# an underfilled target after the splat is nil, on one class and on either of
# two, whose slot held an Integer
class NA; def initialize = (@c = 5); def c = @c; end
class NB; def initialize = (@c = 6); def c = @c; end
one = NA.new
one.instance_eval { @a, *, @c = 1 }
p one.c
ns = [NA.new, NB.new]
ns.each { |o| o.instance_eval { @a, *, @c = 1 } }
p ns.map(&:c)

# a class method's post-splat class-level targets
class Bar
  @n = 1
  def self.go
    a, *b, @n = 1, 2, 3
    [@n, a, b]
  end
end
p Bar.go

# a frozen receiver raises, before the store
begin
  PA.new.freeze.instance_eval { *, @c = 1, 2 }
rescue FrozenError => e
  puts "frozen: #{e.class}"
end
