# A top-level ivar's op-assign used as a value reads the Toplevel slot it
# wrote: the value read named `self`, which the top level does not have, and
# the C did not build. A min/max comparator block is one such value.
@i = 1
x = (@i += 1)
p x
p [1, 2].map { |v| @i += v }
@i = -2
p [11, 12, 22, 33].min { |a, b| @i += 1 }
@i = -2
p [11, 12, 22, 33].max { |a, b| @i += 1 }
@s = "a"
p(@s += "b")
@f = 1.5
p [1, 2].map { |v| @f *= v }

# a boxed slot reads back boxed
@k = "s"
@k = -2
p [11, 12, 22, 33].min { |a, b| @k += 1 }
