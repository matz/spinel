# A ConditionVariable read back boxed (from an Array beside other values)
# answers #signal and #broadcast with itself, also where a user class
# owns #signal.
class Foo
  def signal = :foo
end
x = [ConditionVariable.new, Foo.new, 1][ARGV.size]
m = Mutex.new
m.synchronize { p x.signal.class }
y = [Foo.new, ConditionVariable.new, 1][ARGV.size]
p y.signal
c = [ConditionVariable.new, 1][ARGV.size]
p c.broadcast.class
