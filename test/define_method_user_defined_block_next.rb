# A program with a define_method of its own: that method takes the block as
# a block and may call it, so a `next` in the block stays a `next`. Where the
# block becomes a method's body the `next` is retyped as a `return`
# (test/next_in_run_once_block.rb); done here, the `return` was the enclosing
# method's and `run` did not build. The check is the whole program's, since
# which class a call reaches cannot be read off the class it is written in:
# see Late, Away and Object below.
class Reg
  def define_method(n, &b) = [n, b.call]

  def run
    v = define_method(:a) { next 1 if ARGV.length == 0; 2 }
    [v, :after]
  end

  def self.define_method(n, &b) = b.call

  def self.run
    v = define_method(:b) { next "s" if ARGV.length == 0; "t" }
    [v, :after]
  end
  X = define_method(:c) { next 3 if ARGV.length == 0; 4 }
end
r = Reg.new
p r.run, Reg.run, Reg::X
p r.define_method(:d) { next 5 if ARGV.length == 0; 6 }

# the define_method is written under another name for the class
class Late
  def run
    v = define_method(:h) { next 11 if ARGV.length == 0; 12 }
    [v, :late]
  end
end
L = Late
class L
  def define_method(n, &b) = [n, b.call]
end
p Late.new.run

# the block is run on a Reg from a class that is not Reg
class Away
  def go(o)
    v = o.instance_eval { define_method(:f) { next 7 if ARGV.length == 0; 8 } }
    [v, :away]
  end
end
p Away.new.go(r)

# the call is in a method of Object's, run on a Reg
class Object
  def helper
    v = define_method(:g) { next 9 if ARGV.length == 0; 10 }
    [v, :helper]
  end
end
p r.helper
