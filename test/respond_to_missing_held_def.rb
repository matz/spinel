# A respond_to_missing? defined under a run-time condition: where the branch
# did not run, respond_to? and a direct call reach Kernel's, which answers
# false (and takes two arguments); where it ran, the def answers.
NOT_TAKEN = ARGV.size > 0
TAKEN = ARGV.empty?

# both bodies call super; the def under the condition that did not run
class Quiet
  if NOT_TAKEN
    def method_missing(name, *args) = name == :a ? :mm : super
    def respond_to_missing?(name, include_private = false) = name == :a || super
  end
end
p Quiet.new.respond_to?(:a)

# an optional parameter, no super: the def where it ran, Kernel's where not
class Off
  if NOT_TAKEN
    def respond_to_missing?(n, all = false) = n == :a
  end
end
class On
  if TAKEN
    def respond_to_missing?(n, all = false) = n == :a || super
  end
end
p Off.new.respond_to?(:a), Off.new.respond_to?(:a, true)
p On.new.respond_to?(:a), On.new.respond_to?(:b), On.new.respond_to?(:a, true)

# the same under unless, in two classes
class UnlessOff
  unless TAKEN
    def respond_to_missing?(n, all = false) = n == :a
  end
end
class UnlessOn
  unless NOT_TAKEN
    def respond_to_missing?(n, all = false) = n == :j
  end
end
p UnlessOff.new.respond_to?(:a), UnlessOn.new.respond_to?(:j)

# a receiverless call, fixed and optional parameters
class Ask
  if NOT_TAKEN
    def respond_to_missing?(n, all) = n == :a
  end
  def fixed = respond_to_missing?(:a, false)
end
class AskOn
  if TAKEN
    def respond_to_missing?(n, all) = n == :a
  end
  def fixed = respond_to_missing?(:a, false)
end
class AskOpt
  if NOT_TAKEN
    def respond_to_missing?(n, all = false) = n == :a
  end
  def two = respond_to_missing?(:a, true)
  def one = respond_to_missing?(:a)
end
class AskOptOn
  if TAKEN
    def respond_to_missing?(n, all = false) = n == :a
  end
  def one = respond_to_missing?(:a)
end
p Ask.new.fixed, AskOn.new.fixed, AskOpt.new.two
p AskOptOn.new.one
begin
  AskOpt.new.one
rescue ArgumentError => e
  p e.message
end

# a self receiver, and the explicit-receiver forms
class SelfAsk
  if TAKEN
    def respond_to_missing?(n, all = false) = n == :a
  end
  def t = [respond_to?(:a), self.respond_to?(:a), respond_to?(:b)]
end
p SelfAsk.new.t
p SelfAsk.new.send(:respond_to_missing?, :a, false), Off.new.send(:respond_to_missing?, :a, false)

# a held def in a base class, asked on a subclass with its own def and a sibling
class Base
  if TAKEN
    def respond_to_missing?(n, all = false) = n == :base || super
  end
end
class Sub < Base
  def respond_to_missing?(n, all = false) = n == :sub
end
class Sib < Base; end
p Base.new.respond_to?(:base), Sub.new.respond_to?(:base), Sub.new.respond_to?(:sub), Sib.new.respond_to?(:base), Sib.new.respond_to?(:other)

# private
class Hidden
  if TAKEN
    private
    def respond_to_missing?(n, all = false) = n == :a
  end
end
h = Hidden.new
p h.respond_to?(:a), h.respond_to?(:b)
begin
  h.respond_to_missing?(:a)
rescue NoMethodError => e
  p e.message
end

# one object kind among others
class Other; end
xs = [On.new, Other.new, 1, "s"]
p xs.map { |x| x.respond_to?(:a) }

# a receiver of two classes, one holding the def and one not
class HeldTaken
  if TAKEN
    def respond_to_missing?(n, all = false) = n == :a
  end
end
class HeldOff
  if NOT_TAKEN
    def respond_to_missing?(n, all = false) = n == :a
  end
end
x = TAKEN ? HeldTaken.new : HeldOff.new
y = TAKEN ? HeldOff.new : HeldTaken.new
p x.respond_to?(:a), y.respond_to?(:a)

# a public list of methods names no respond_to_missing?, which is private
p HeldOff.instance_methods(false), HeldOff.new.methods.include?(:respond_to_missing?)

# counts the def takes and Kernel's takes not: each reaches its own ArgumentError
class Counts
  if TAKEN
    def respond_to_missing?(n, all = false) = n == :a
  end
  def zero = respond_to_missing?
  def three = respond_to_missing?(:a, true, 3)
end
class CountsOff
  if NOT_TAKEN
    def respond_to_missing?(n, all = false) = n == :a
  end
  def zero = respond_to_missing?
  def three = respond_to_missing?(:a, true, 3)
end
[Counts.new, CountsOff.new].each do |o|
  begin; o.zero; rescue ArgumentError => e; p e.message; end
  begin; o.three; rescue ArgumentError => e; p e.message; end
end
