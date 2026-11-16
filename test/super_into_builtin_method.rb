# A super that no method of the program answers reaches the builtin's own
# method, whatever shape the arguments and the block take. A Kernel module
# function (format, sprintf, puts, p) is called bare, as Kernel.format is; an
# Array, Hash or String subclass's method reaches the builtin's with its
# parameters passed on by CRuby's rule: a rest as a splat, posts, keywords as
# keywords, a keyword rest as **, the anonymous * ** and & as themselves, and
# the method's block unless the super gives its own or &nil.

class Fmt
  def format(f, n) = "<" + super + ">"
  def sprintf(f, n, s) = "[" + super + "]"
  def puts(a, b)
    super
    :done
  end
  def p(a, b) = super
  def run
    $stdout.puts format("%05.1f", 3.14159)
    $stdout.puts sprintf("%d-%s", 5, "z")
    p(1, 2)
    $stdout.puts puts("x", "y").inspect
  end
end
Fmt.new.run

class Twice
  def puts(x) = super(x.to_s * 2)
  def format(f, x, &b) = super
  def go = format("%s", :x) { 1 }
end
Twice.new.puts(7)
puts Twice.new.go

module Loud
  def puts(a, b) = super(a.upcase, b.upcase)
end
class Shout
  include Loud
  def go = puts("hi", "there")
end
Shout.new.go

class Post < Array
  def push(first, *mid, last) = super
end
a = Post.new
a.push(1, 2, 3, 4)
a.push(5, 6)
p a

class Packer < Array
  def pack(fmt, buffer: nil) = super
end
pk = Packer.new([65, 66])
p pk.pack("C*")
p pk.pack("C*", buffer: +"x")

class Anon < Array
  def push(*) = super
  def each(&) = super
end
an = Anon.new
an.push(1, 2)
an.push(3)
seen = []
an.each { |x| seen << x }
p seen
p an.each.to_a

class Bytes < String
  def unpack1(fmt, offset: 0) = super
  def center(w, pad = " ") = super + "|"
end
s = Bytes.new("abcd")
p s.unpack1("C")
p s.unpack1("C", offset: 2)
p s.center(8)
p s.center(8, "*")

class Opt < Hash
  def fetch(k, *d, &b) = super
end
h = Opt.new
h[:a] = 1
p h.fetch(:a)
p h.fetch(:z, 0)
p h.fetch(:q) { |k| k.to_s * 2 }

class Summer < Array
  def sum(init = 0, &blk) = super + 1
  def each(&b) = super(&nil)
  def map(&b) = super(&b)
end
sm = Summer.new([1, 2, 3])
p sm.sum
p sm.sum(10)
p sm.sum { |x| x * 2 }
p sm.each { |x| raise "no" }.class
p sm.map { |x| x * 2 }
p sm.map.class

module Wrap
  def push(*a) = super
end
class Base
  def push(*a) = [:base, a]
end
class ViaBase < Base
  include Wrap
end
class ViaArray < Array
  include Wrap
end
p ViaBase.new.push(1, 2)
va = ViaArray.new
va.push(1, 2)
p va

module Fwd
  def each_slice(...) = super
  def each(...) = super
end
class Slicer < Array
  include Fwd
end
sl = Slicer.new([1, 2, 3])
p sl.each_slice(2).to_a
doubled = []
p(sl.each { |x| doubled << x * 2 }.size)
p doubled

# a Kernel function that reads the calling frame is not the super's to answer; the override only has to build
class Frame
  def __method__ = super
  def caller_name = :frame
end
p Frame.new.caller_name
