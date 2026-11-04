# A class below SystemExit was built as any other exception: its first argument
# was the message, never the status, and super(status, msg) raised TypeError.
# Uncaught, it printed an error and exited 1. A boxed one answered neither
# #status nor #success?. CRuby takes SystemExit#initialize's arguments for it
# and ends the program quietly with its status.
class Stop < SystemExit; end
class Deeper < Stop; end
class Tagged < SystemExit
  attr_accessor :tag
end
class Halt < SystemExit
  def initialize(code)
    super(code, "halted #{code}")
  end
end
class Forwarding < SystemExit
  def initialize(code)
    super
  end
end
class Quiet < SystemExit
  def initialize
    super(false)
  end
end
class Named < SystemExit
  def initialize(why)
    super(why)
  end
end
class Built < SystemExit
  def self.build(n) = new(n, "built")
end

def show(label, e)
  puts "#{label}: #{e.class} status=#{e.status} success=#{e.success?} msg=#{e.message} #{e.is_a?(SystemExit)}"
end

show("no argument", Stop.new)
show("integer", Stop.new(11))
show("true", Stop.new(true))
show("false", Stop.new(false))
show("integer and message", Stop.new(2, "bye"))
show("true and message", Stop.new(true, "bye"))
show("message", Stop.new("bye"))
show("empty message", Stop.new(3, ""))
show("nil", Stop.new(nil))
show("float", Stop.new(1.5))
show("symbol", Stop.new(:sym))
show("deeper", Deeper.new(4))
show("super with both", Halt.new(5))
show("bare super", Forwarding.new(6))
show("super with a boolean", Quiet.new)
show("super with a message", Named.new("why"))
show("a class method's new", Built.build(7))
klass = Stop
show("a class in a variable", klass.new(8, "held"))
show("SystemExit itself", SystemExit.new(1.5))

tagged = Tagged.new(9, "tagged")
tagged.tag = "kept"
show("with an ivar", tagged)
puts tagged.tag

begin
  Stop.new("a", "b")
rescue ArgumentError => e
  puts e.message
end
begin
  SystemExit.new(1, "a", "b")
rescue ArgumentError => e
  puts e.message
end

begin
  raise Stop
rescue Stop => e
  show("raise the class", e)
end
begin
  raise Stop, "text"
rescue SystemExit => e
  show("raise with a message", e)
end
begin
  raise Stop, 12
rescue SystemExit => e
  show("raise with a status", e)
end
begin
  raise SystemExit, 13
rescue SystemExit => e
  show("raise SystemExit with a status", e)
end
begin
  raise Stop.new(14), "replaced"
rescue SystemExit => e
  show("raise an instance with a message", e)
end

# not a StandardError, so a bare rescue lets it through
begin
  begin
    raise Stop.new(15)
  rescue
    puts "a bare rescue must not catch it"
  end
rescue Exception => e
  show("past a bare rescue", e)
end

# any other exception in the same slot still has no #status
[Stop.new(16), Halt.new(17), RuntimeError.new("x")].each do |e|
  begin
    puts e.status
  rescue NoMethodError => x
    puts x.message
  end
end

# Uncaught, it ends the program as an exit does.
at_exit { puts "at_exit ran" }
begin
  raise Stop.new(0)
ensure
  puts "ensure ran"
end
puts "must not print"
