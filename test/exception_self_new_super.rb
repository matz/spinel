# An exception class whose own self.new returns another exception for one
# case and calls super otherwise (net-ldap's ConnectionError.new), and a
# subclass whose self.new reaches it through super
class Err < StandardError; end
class ConnErr < Err
  def self.new(errors)
    return Err.new(errors.first) if errors.size == 1
    super
  end
  def initialize(errors)
    super("failed: #{errors.join(", ")}")
  end
end
class Timeout < ConnErr
  def self.new(errors)
    super(errors + ["timeout"])
  end
end
p ConnErr.new(["a"])
p ConnErr.new(["a", "b"])
p ConnErr.new(["a", "b"]).class
t = Timeout.new(["x"])
p t, t.class
begin
  raise ConnErr.new(["p", "q"])
rescue Err => e
  p [e.class, e.message]
end
begin
  raise Timeout.new(["r"])
rescue ConnErr => e
  p [e.class, e.message]
end

# with no initialize of its own: Exception#initialize takes the message
class Explicit < StandardError
  def self.new(message) = super(message)
end
class Bare < StandardError
  def self.new(message) = super
end
class Plain < StandardError
  def self.new(message) = super()
end
class Two < StandardError
  def self.new(a, b) = super
end
class Coded < StandardError
  attr_accessor :code
  def self.new(message, code)
    err = super(message)
    err.code = code
    err
  end
end
class Missing < KeyError
  def self.new(message) = super
end
p Explicit.new("explicit").message, Bare.new("bare").message, Plain.new("plain").message
p Bare.new(:sym).message, Bare.new(nil).message
begin
  Two.new("a", "b")
rescue ArgumentError => e
  p e.message
end
coded = Coded.new("coded", 7)
p coded.message, coded.code, coded.class
p Missing.new("missing").message, Missing.new("missing").class
begin
  raise Bare, "raised"
rescue Bare => e
  p [e.class, e.message]
end

# a splat's argument runs once for the receiving class, and a bare super
# spreads a rest parameter (a block parameter Exception#initialize ignores)
class Shifted < StandardError
  def self.new(messages) = super(*[messages.shift])
end
class ShiftedChild < Shifted; end
p ShiftedChild.new(["c", "d"]).message, Shifted.new(["b", "e"]).message
class Rest < StandardError
  def self.new(*args) = super
end
p Rest.new("m").message, Rest.new.message
class Mixed < StandardError
  def self.new(first, *more) = super
end
p Mixed.new("f").message
begin
  Mixed.new("f", "g")
rescue ArgumentError => e
  p e.message
end
class WithBlock < StandardError
  def self.new(*args, &blk) = super
end
p WithBlock.new("w") { 1 }.message
