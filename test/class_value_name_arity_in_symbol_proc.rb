# Module#name, to_s and inspect take no argument: a call with one on a class
# value raises the arity ArgumentError after reading the arguments. Where a
# symbol proc (&:name) hands a class value and a second argument to a poly
# dispatch whose slot is boxed, the raise's statement expression (valued as
# the reader's bytes, NULL) is boxed as the reader's answer is. A large
# application program's `columns.map(&:name)` beside a two-argument use of
# the same proc wrote the C string into the boxed slot.
class Col
  def initialize(n) = @n = n
  def name = @n
end
class Reg
  def initialize(n) = @n = n
  protected
  def name = @n
end
class Mod
  def self.name = "mod!"
end
xs = [Col.new("a"), Col.new("b")]
xs << Reg.new("r") if ARGV.size > 3
xs << Mod if ARGV.size > 4
xs << String if ARGV.size > 5
p xs.map(&:name)
pr = :name.to_proc
begin
  p pr.call(xs[ARGV.size % 2], 1)
rescue ArgumentError, NoMethodError => e
  puts e.message
end
k = [String, Mod][ARGV.size % 2]
o = [Col.new("c"), k][ARGV.size % 2]
p o.name
begin
  p o.name(1)
rescue ArgumentError => e
  puts e.message
end
begin
  p k.to_s(2)
rescue ArgumentError => e
  puts e.message
end
