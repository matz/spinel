# x&.name, x&.to_s and x&.inspect on a Class value that may be nil (a
# nil-defaulted parameter, an ivar, a method's answer) answer nil for the
# nil and the reader's result for a class.
class K; end
module M; end
def f(x = nil) = x&.name
p f
p f(K)
p f(M)
def g(x = nil) = x&.to_s
p g
p g(String)
def i(x = nil) = x&.inspect
p i
p i(K)
def pick(b) = b ? K : nil
p pick(false)&.name
p pick(true)&.name
class Holder; def initialize(k) = @k = k; def nm = @k&.name; end
p Holder.new(nil).nm
p Holder.new(K).nm
def sup(x = nil) = x&.superclass
p sup
p sup(K)
def nn(x = nil) = x&.name&.length
p nn
p nn(K)

# The first safe navigation keeps the Class boxed when the input is boxed.
def boxed_name(value) = value&.class&.name
items = [+"string", 1, nil]
p boxed_name(items[0]), boxed_name(items[1]), boxed_name(items[2])
