# A method of a class that is inlined into an instance_eval block (a
# forwarder or a yielding method called there) reads its own object's ivars,
# not the rebound receiver's.
class Root; def initialize = (@a = 4); end
class Fwd
  def initialize = (@root = Root.new)
  def build(&b) = @root.instance_eval(&b)
end
class A; def initialize = (@a = 1); end
class W
  def initialize = (@root = :w)
  def go(a) = a.instance_eval { [@a, Fwd.new.build { @a }] }
end

# a forwarder spliced in the block, at the top level and in a method
p A.new.instance_eval { [@a, Fwd.new.build { @a }] }
p W.new.go(A.new)

# a plain method and a yielding one reading their own ivars
class Tw
  def initialize = (@root = 7)
  def root = @root
  def twice(&b) = [@root, yield]
end
p A.new.instance_eval { [@a, Tw.new.root, Tw.new.twice { @a }] }

# the same through a yielding method whose ivar is a String, an Integer
class Sw
  def initialize = (@name = "fwd")
  def with = [@name, yield]
end
class Iw
  def initialize = (@root = 7)
  def twice = [@root, yield]
end
p A.new.instance_eval { Sw.new.with { @a } }
p A.new.instance_eval { Iw.new.twice { @a } }
