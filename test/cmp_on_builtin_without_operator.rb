# `<`, `<=`, `>`, `>=` on a value typed as a builtin whose class has no such
# method (true, false, an Exception, a Regexp, a Proc, a MatchData): CRuby's
# NoMethodError when the call runs, after its argument. It was refused at
# compile time, so a method that only a computed send could reach with such a
# value failed the build even when the program never called it that way.

def try
  yield
rescue NoMethodError => e
  puts e.message
end

def exc_ge(x) = x >= 0
def exc_lt(x) = x < side_effect
def bool_le(x) = x <= 1
def bool_gt(x) = x > 1
def re_ge(x) = x >= 0
def proc_lt(x) = x < 0
def md_gt(x) = x > 0

def side_effect
  puts "argument runs first"
  0
end

try { p exc_ge(RuntimeError.new("boom")) }
try { p exc_lt(ArgumentError.new("a")) }
try { p bool_le(true) }
try { p bool_gt(false) }
try { p re_ge(/re/) }
try { p proc_lt(proc { 1 }) }
try { p md_gt("abc".match(/b/)) }
begin
  raise "x"
rescue => e
  try { p e > 1 }
end

# A handler table: the computed send may pass the error to any one-argument
# method, so `seconds` is typed as an Exception too.
class Handler
  attr_reader :method_name
  def initialize(method_name)
    @method_name = method_name
  end
end

class Controller
  HANDLERS = [Handler.new(:not_found)]

  def expires_in(seconds)
    seconds.is_a?(Integer) && seconds >= 0
  end

  def not_found(error)
    "handled #{error.message}"
  end

  def handle(error)
    HANDLERS.each { |h| return send(h.method_name, error) }
    nil
  end
end

begin
  raise "missing"
rescue => e
  p Controller.new.handle(e)
end
p Controller.new.expires_in(60)
