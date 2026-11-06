# Exception's class-level `exception` builds what `new` builds, on any class of
# Exception: by name, through self and on a class held in a value.
class Plain < StandardError; end
class Below < Plain; end
class Two < StandardError
  attr_reader :n
  def initialize(n = 7, msg = "two")
    super(msg)
    @n = n
  end
end
class Stop < SystemExit; end
class Halt < SystemExit
  def initialize(n)
    super(n, "halt #{n}")
  end
end
class Bare < StandardError
  def self.bare(m) = exception(m)
  def self.on_self(m) = self.exception(m)
  IN_BODY = exception("in the body")
end
class BareBelow < Bare; end
module Space
  class Inner < StandardError; end
  class RuntimeError < StandardError; end
  def self.lexical = RuntimeError.exception("lexical")
end
class Hidden < StandardError
  private_class_method :new
end
class Kw < StandardError
  attr_reader :k
  def initialize(msg = nil, k: 1)
    super(msg)
    @k = k
  end
end
class Opt < StandardError
  attr_reader :o
  def initialize(msg = nil, o = nil)
    super(msg)
    @o = o
  end
end
class Blk < StandardError
  attr_reader :b
  def initialize(m = nil, &b)
    super(m)
    @b = b
  end
end

def show(e)
  puts "#{e.class} #{e.message.inspect}"
end

def arity
  yield
  puts "no error"
rescue ArgumentError => e
  puts e.message
end

# a class by name, with and without a message
show Plain.exception
show Plain.exception("hi")
show Below.exception("below")
show Plain.exception(nil)
show Two.exception
show Two.exception(3)
show Two.exception(3, "x")
puts Two.exception(3).n
arity { Two.exception(1, 2, 3) }
show Space::Inner.exception("path")
show Space::RuntimeError.exception("shadow")
show Space.lexical
ALIASED = Plain
show ALIASED.exception("aliased")

# the builtin classes
show StandardError.exception
show RuntimeError.exception("r")
show ArgumentError.exception("bad")
show KeyError.exception("k")
show NoMethodError.exception("nm")
show FrozenError.exception("fz")
show Errno::ENOENT.exception
show Errno::ENOENT.exception("path")
show SystemCallError.exception("sc", 2)
show Interrupt.exception("int")
show SignalException.exception("INT")

# SystemExit reads its status as new reads it
puts SystemExit.exception.status
puts SystemExit.exception(3).status
puts SystemExit.exception(3).message
puts SystemExit.exception(false).status
puts SystemExit.exception(true).success?
puts SystemExit.exception("m").status
puts SystemExit.exception("m").message
puts SystemExit.exception(2, "m").message
puts Stop.exception(4).status
puts Halt.exception(5).status
puts Halt.exception(5).message
begin
  raise Stop.exception(6)
rescue SystemExit => e
  puts e.status
end

# through self: the class that receives the call, in a method and in a body
show Bare.bare("x")
show Bare.on_self("y")
show BareBelow.bare("x below")
show BareBelow.on_self("y below")
show Bare::IN_BODY

# a private new stops new alone
show Hidden.exception("hidden")

# keywords reach initialize as the positional Hash
arity { Kw.exception("kw", k: 2) }
puts Kw.exception("kw").k
puts Kw.new("kw", k: 2).k
p Opt.exception("kw", k: 2).o
p Blk.exception("b") { 5 }.b.call

# a class value only the run time knows
def pick(which) = which ? Plain : Two
show pick(true).exception("picked")
show pick(false).exception
def build(k) = k.exception("x")
show build(Plain)
show build(RuntimeError)
begin
  raise Two, "first"
rescue Two => e
  show e.class.exception(9)
end
class Holder
  def initialize(k) = @k = k
  def make(m) = @k.exception(m)
end
show Holder.new(Plain).make("ivar")
show Holder.new(RuntimeError).make("ivar too")
begin
  build(String)
rescue NoMethodError => e
  puts e.message
end
begin
  Holder.new(String).make("x")
rescue NoMethodError => e
  puts e.message
end

# a boxed value that holds a class, an instance or something else
[Plain, Two].each { |k| show k.exception }
[Plain, RuntimeError].each { |k| show k.exception("m") }
puts [Plain, Two, SystemExit].map { |k| k.exception.class }.inspect
puts [SystemExit, Stop, Halt].map { |k| k.exception(7).status }.inspect
{ a: Plain, b: RuntimeError }.each { |n, k| show k.exception("from #{n}") }
[Plain, Plain.new("instance")].each { |x| show x.exception("either") }
[Plain.new("i"), RuntimeError.new("j")].each do |x|
  show x.exception
  show x.exception("k")
  puts x.exception.equal?(x)
end
$asked = 0
def next_class
  $asked += 1
  [Plain, RuntimeError][$asked % 2]
end
show next_class.exception("once")
puts $asked
begin
  [Plain, String].last.exception("s")
rescue NoMethodError => e
  puts e.message
end
begin
  [Plain, 5].last.exception
rescue NoMethodError => e
  puts e.message
end
puts [nil, Plain].first&.exception("safe").inspect
show [nil, Plain].last&.exception("safe")

# the name reached by a send, and asked about
show Plain.send(:exception, "sent")
show Plain.public_send(:exception, "public")
name = :exception
[Plain, RuntimeError].each { |k| show k.send(name, "computed") }
show Two.send(name, 8)
puts Plain.respond_to?(:exception)
puts RuntimeError.respond_to?(:exception)
puts String.respond_to?(:exception)
puts Object.new.respond_to?(:exception)

# the instance's #exception is another method
orig = Plain.new("orig")
puts orig.exception.equal?(orig)
other = orig.exception("other")
puts other.equal?(orig)
show other
two = Two.exception(9)
copy = two.exception("next")
puts copy.class, copy.n, copy.message

begin
  raise Plain.exception("raised")
rescue Plain => x
  show x
end
begin
  begin
    raise Two.exception(2)
  rescue Two => x
    raise x.class.exception(x.n + 1)
  end
rescue Two => y
  puts y.n
end
