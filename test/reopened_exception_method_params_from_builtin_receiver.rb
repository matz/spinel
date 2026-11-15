# A method a program adds to a builtin exception class takes its
# parameters' types from every call, also one whose receiver is a builtin
# exception (ArgumentError.new, a rescued exception): the plan picks among
# the reopenings that define the name by the class name at run time, so
# each one's parameters take the call's arguments. Bound only by the call
# of a program subclass, the builtin receiver's argument went into a slot
# of the subclass call's type: a wrong value, or a refusal.
class StandardError
  def echo(v) = v
end
class StandardError
  def twice(v) = v * 2
  def opt(a, b = 10, k: :kw) = [a, b, k]
end
class RuntimeError
  def which(x) = [:runtime, x]
end
class ArgumentError
  def which(x) = [:argument, x]
end
class MyErr < StandardError; end
p MyErr.new("m").twice(3)
p ArgumentError.new("a").twice("ab")
p MyErr.new("m").opt(1)
p TypeError.new("t").opt("s", 2.5, k: nil)
p RuntimeError.new("r").which(1)
p ArgumentError.new("a").which("s")
begin
  raise IndexError, "i"
rescue => e
  p e.twice([1])
end
p MyErr.new("m").echo(:s)
p ArgumentError.new("a").echo(1)
p RuntimeError.new("r").echo(2.5)
