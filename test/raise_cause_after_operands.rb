# An explicit `cause:` belongs to the exception the raise names. A raise
# inside an operand does not take it, an operand's rescued raise does not
# drop it, and the TypeError raise makes for a wrong operand does not carry
# it: that TypeError's cause is the exception being handled.
C = RuntimeError.new("c")
def show
  yield
rescue => e
  p [e.class, e.message, e.cause&.message]
end

# the TypeError for an operand that is not an exception
show { raise(5, cause: C) }
show { raise("m", {x: 1}, cause: C) }
show { raise(ArgumentError, "m", {x: 1}, cause: C) }
show { raise(RuntimeError, "m", 5, cause: C) }
x = [5, "s"].first
show { raise(x, cause: C) }
y = [Object.new, 1].first
show { raise(y, cause: C) }
def mk(f) = f ? RuntimeError.new("r") : 5
show { raise(mk(false), cause: C) }
s = [String, ArgumentError].first
show { raise(s, "m", cause: C) }
begin
  raise "outer"
rescue
  show { raise(5, cause: C) }
  show { raise(5, cause: nil) }
  show { raise(RuntimeError, "m", cause: nil) }
  show { raise(RuntimeError, "m", cause: C) }
end

# a raise inside an operand
def bar = raise(KeyError, "k")
show { raise(RuntimeError, bar, cause: C) }
def soft = (raise "x" rescue "m")
show { raise(RuntimeError, soft, cause: C) }
show { raise(RuntimeError, "m", cause: (raise "y" rescue C)) }

# an initialize that rescues its own raise
class MyErr < StandardError
  def initialize(m = "my")
    begin
      raise "inside"
    rescue
    end
    super
  end
end
show { raise(MyErr, "m", cause: C) }
show { raise(MyErr, cause: C) }
k = [MyErr, String].first
show { raise(k, "m", cause: C) }
show { raise(k, cause: C) }

# the right raises keep their cause
show { raise(ArgumentError, "a", cause: C) }
show { fail(ArgumentError, "a", cause: C) }
show { raise("str", cause: C) }
show { raise(NoMethodError.new("nm", :zz, [1, 2]), cause: C) }
v = [C, 1].first
show { raise(RuntimeError, "m", cause: v) }
e1 = RuntimeError.new("e1")
show { raise(e1, "m2", cause: C) }
def pick(f) = f ? 1 : raise(ArgumentError, "bad", cause: RuntimeError.new("c2"))
show { pick(false) }
p pick(true)

# a class or an exception known only at run time: the message's to_s and
# the signal name run before the cause is staged
class M; def to_s; (raise "inner" rescue nil); "mm"; end; end
j = [ArgumentError, String].first
show { raise(j, M.new, cause: C) }
e0 = [RuntimeError.new("o"), 1].first
show { raise(e0, M.new, cause: C) }
sg = [SignalException, String].first
show { raise(sg, "BOGUS", cause: C) }
begin
  raise(sg, "INT", cause: C)
rescue SignalException => e
  p [e.message, e.cause&.message]
end

# a SystemCallError class known only at run time builds from the operand
# itself: a lone Integer is the errno
c9 = RuntimeError.new("c9")
[SystemCallError, Errno::ENOENT, RuntimeError].each do |k|
  [2, "boom", nil].each do |m|
    begin
      raise k, m, cause: c9
    rescue Errno::ENOENT => e
      p [:enoent, e.class, e.message, e.cause]
    rescue Exception => e
      p [e.class, e.message, e.cause]
    end
  end
end
