# A builtin class's arm of a call split on a boxed class value
# (`k.equal?(TCPServer) ? TCPServer.new(x, y) : k.new(x, y)`) that raises
# for an argument it can't convert evaluates every argument first, as
# CRuby does: `marker` prints before the conversion and its TypeError.

require "socket"
class Wrong
  def to_str
    puts "to_str"
    42
  end
end
class Missing; end
class Base
  def initialize(a, b) = @a = a
end
def marker
  puts "marker"
  0
end
def build_wrong(klass)
  klass.new(Wrong.new, marker).class.name
rescue TypeError => e
  "#{klass}: #{e.message}"
end
def build_missing(klass)
  klass.new(Missing.new, marker).class.name
rescue TypeError => e
  "#{klass}: #{e.message}"
end
[Base, TCPServer].each { |klass| puts build_wrong(klass) }
[Base, TCPServer].each { |klass| puts build_missing(klass) }
