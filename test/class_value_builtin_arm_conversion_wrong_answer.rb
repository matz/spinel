# A builtin class's arm of a call split on a boxed class value
# (`k.equal?(Array) ? Array.new(x) : k.new(x)`) whose argument's conversion
# method answers the wrong kind runs the method, then raises CRuby's
# TypeError where the arm runs, rather than refusing the program.

class Req
  def to_int
    puts "to_int"
    "nope"
  end

  def to_str
    puts "to_str"
    42
  end
end

class Base
  def initialize(request) = @request = request
end

def build(klass)
  klass.new(Req.new).class.name
rescue TypeError => e
  "#{klass}: #{e.class}: #{e.message}"
end

[Base, Array, String].each { |klass| puts build(klass) }
