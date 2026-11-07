# A `super` in a class's own `def self.exception` that no class method of
# the program above answers reaches Exception.exception, which builds the
# receiving class through allocate and initialize, as `new` does.
class AppError < StandardError; end
def try
  yield
rescue => e
  p [e.class, e.message]
end
class A < StandardError
  def self.exception(m = "d") = super("a:" + m)
end
class B < A
  def self.exception(m = "d") = super("b:" + m)
end
try { raise B, "x" }
try { raise A, "x" }
try { raise B }
x = B.exception("q")
p [x.class, x.message]
class C < B
  def self.exception(m = "d") = super
end
try { raise C, "x" }
# a super taken on one branch only
class Cond < StandardError
  def self.exception(m = "d")
    return super("cond:" + m) if m.start_with?("y")
    AppError.new("app:" + m)
  end
end
class CondSub < Cond
  def self.exception(m = "d") = super("s" + m)
end
try { raise CondSub, "x" }
try { raise Cond, "yes" }
try { raise Cond, "no" }
# class << self, a builtin parent other than StandardError, rescue by parent
class Arg < ArgumentError
  class << self
    def exception(m = "d") = super("arg:" + m)
  end
end
begin
  raise Arg, "z"
rescue ArgumentError => e
  p [:arg, e.class, e.message]
end
# the arguments go to initialize, as the super passes them
class Two < StandardError
  def initialize(a, b = "b") = super("#{a}/#{b}")
  def self.exception(a, b = "q") = super(a, b)
end
class TwoSub < Two
  def self.exception(m) = super("t" + m)
end
try { raise TwoSub, "x" }
class Kw < StandardError
  def initialize(msg = "e", code: 1) = super("#{msg}:#{code}")
  def self.exception(m = "d") = super(m, code: 7)
end
try { raise Kw, "x" }
# a class built only through its own exception has its methods
class Only < StandardError
  def self.exception(m = "d") = super("only:" + m)
  def code = "str"
end
class Other
  def code = 1
end
def pick(i) = i == 0 ? Only.exception("x") : Other.new
p pick(0).code
p pick(1).code
p pick(0).message
