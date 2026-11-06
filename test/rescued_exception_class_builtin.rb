# A rescued exception of a builtin class answers #class with that class, known
# by its name: what it builds and calls is that class's, not the program's
# first class's.
class First
  def initialize(a = 0) = @a = a
  def self.tag(m) = "First #{m}"
end
class Boom < StandardError
  def self.tag(m) = "Boom #{m}"
end
class Below < Boom; end
class Stat < StandardError
  attr_reader :n

  def initialize(n = 1)
    super("n=#{n}")
    @n = n
  end
end

def show(e)
  puts "#{e.class} #{e.message.inspect}"
end

# the class itself, and what is asked of it
begin
  raise KeyError, "k"
rescue => e
  puts e.class
  puts e.class.name
  puts e.class == KeyError
  puts e.class == Boom
  puts e.class == First
  puts e.class.superclass
  puts e.class.ancestors.include?(IndexError)
  puts e.class.ancestors.include?(Boom)
  puts e.class <= IndexError
  puts e.class <= Boom
  puts e.class.respond_to?(:tag)
  show e.class.new("y")
  show e.class.exception("z")
  k = e.class
  puts k
  puts k == KeyError
  show k.new("yy")
  show k.exception("zz")
  begin
    k.tag("no")
  rescue NoMethodError => r
    puts r.class
  end
end

# each way a builtin exception comes to be rescued
begin
  raise "plain"
rescue => e
  show e.class.new("a")
end
begin
  raise TypeError, "t"
rescue TypeError => e
  show e.class.new("b")
  show e.class.exception
end
begin
  1 / 0
rescue ZeroDivisionError => e
  show e.class.new("c")
end
begin
  Integer("x")
rescue ArgumentError => e
  show e.class.exception("d")
end
begin
  [].fetch(3)
rescue IndexError => e
  show e.class.new("e")
end
begin
  "s".frozen_thing
rescue NoMethodError => e
  show e.class.new("f")
end
begin
  raise Errno::ENOENT, "g"
rescue SystemCallError => e
  show e.class.new("h")
  show e.class.exception("i")
  puts e.class.superclass
end
begin
  raise SystemExit.new(4)
rescue SystemExit => e
  puts e.class.new(5).status
  puts e.class.exception(6).status
end

# through a parameter, $! and #cause
def rebuild(e) = e.class.new("again")
begin
  raise IOError, "io"
rescue => e
  show rebuild(e)
end
begin
  raise Boom, "b"
rescue => e
  show rebuild(e)
end
begin
  raise RangeError, "r"
rescue
  show $!.class.new("from $!")
end
begin
  begin
    raise EOFError, "first"
  rescue
    raise "second"
  end
rescue => e
  show e.cause.class.new("from cause")
end

# the program's classes keep theirs
begin
  raise Boom, "b"
rescue => e
  show e.class.new("y")
  show e.class.exception("z")
  puts e.class.tag("kept")
  puts e.class == Boom
  puts e.class.superclass
end
begin
  raise Below, "c"
rescue Boom => e
  show e.class.new("y")
  puts e.class.tag("inherited")
  puts e.class.superclass
end
begin
  raise Stat.new(2)
rescue => e
  show e.class.new(3)
  puts e.class.new(4).n
end

# nil's class, outside any rescue
puts $!.class
begin
  $!.class.new("nil")
rescue NoMethodError => r
  puts r.message
end
