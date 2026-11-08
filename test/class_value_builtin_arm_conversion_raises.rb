# `k.new(x)` on a boxed class value splits into an arm per builtin class
# the program reads as a value (`k.equal?(Array) ? Array.new(x) : ...`).
# An argument such an arm can't convert raises TypeError when the arm runs,
# as CRuby does, rather than refusing the program: the value may never be
# that class (`Object.const_get(name)` with the program's own classes).

class Req; end

class Base
  def initialize(request) = @request = request
  def index = "index"
end

class Pair
  def initialize(a, b) = @a = a
end

def build(klass)
  controller = klass.new(Req.new)
  controller.public_send("index".to_sym)
rescue TypeError => e
  "#{klass}: #{e.class}: #{e.message}"
end

def build2(klass)
  klass.new(Req.new, Req.new).class.name
rescue TypeError => e
  "#{klass}: #{e.class}: #{e.message}"
end

puts build(Object.const_get(ARGV[0] || "Base"))
[Base, Array, String].each { |klass| puts build(klass) }
puts build2(Object.const_get(ARGV[0] || "Pair"))
[Pair, Array].each { |klass| puts build2(klass) }
