# `g.method(n)` with a computed name reaches the methods of g's class
# alone (and Object's, which it inherits), so the share walk joins those
# with what it does not see, not every method of the program (#9627).
class Greeter
  def hello(x) = "hello #{x}"
  def bye(x) = "bye #{x}"
end
g = Greeter.new
n = [:hello, :bye][ARGV.size]
m = g.method(n)
puts m.call("w")

class Store
  def put(env = ENV) = env["HOME"]
end
def shout(s) = s.upcase!
t = +"hi"
shout(t)
p t, Store.new.put.nil?
