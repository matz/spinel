# A program's own Object method whose name ends in `?` answers what its body
# returns. activesupport's Object#acts_like? returns respond_to?'s answer,
# which a class's own respond_to? may give as any value. Called on an
# Integer or a program object that does not define it, the call was typed a
# Boolean by its name alone, and the method's boxed answer was read as one:
# the C did not compile.
class Object
  def acts_like?(duck)
    case duck
    when :time then respond_to? :acts_like_time?
    else respond_to? :"acts_like_#{duck}?"
    end
  end
end
class Weird
  def respond_to?(m, priv = false) = m == :acts_like_time? ? :yes : nil
end
class Clock
  def acts_like_time? = true
end
x = [Clock.new, Weird.new, 3][ARGV.size]
p x.acts_like?(:time)
p 3.acts_like?(:time), Weird.new.acts_like?(:time)
