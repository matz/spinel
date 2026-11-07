# `k.exception(a)` on a class value: a class of the program with its own (or
# an inherited, or an extended) class method `exception` runs it; every
# other class builds through new.
class AppError < StandardError
  def initialize(m = "default") = super("App:#{m}")
end
class Wrapped < StandardError
  def self.exception(m = "none") = AppError.new("wrapped #{m}")
end
class WrappedKid < Wrapped; end
class Says < StandardError
  def self.exception(m = "none") = RuntimeError.new("says #{m}")
end
module Builder
  def exception(m = "none") = ArgumentError.new("built #{m}")
end
class Extended < StandardError; extend Builder; end
class Plain
  def self.exception(m) = "plain #{m}"
end
module NS
  class Same < StandardError
    def self.exception(m = nil) = StandardError.new("NS::Same #{m}")
  end
end
class Same < StandardError; end

def build(k, m) = k.exception(m)
def build0(k) = k.exception

[AppError, Wrapped, WrappedKid, Says, Extended, NS::Same, Same, ArgumentError, RuntimeError].each do |k|
  e = build(k, "x")
  p [k, e.class, e.message]
  e = build0(k)
  p [k, e.class, e.message]
end

[Plain, Same, Same.new("inst")].each do |v|
  r = v.exception("m")
  p [r.class, r.is_a?(String) ? r : r.message]
end

inst = AppError.new("i")
[Wrapped, inst].each do |v|
  r = build(v, "o")
  p [r.class, r.message, r.equal?(inst)]
end
p build0(inst).equal?(inst)

# an instance whose class defines its own == takes its own exception, and
# its == does not run against the classes
class Eq < StandardError
  def ==(o) = true
end
[Eq.new("e"), IOError].each { |v| r = build(v, "z"); p [r.class, r.message] }
