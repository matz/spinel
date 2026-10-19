# A send with a computed name whose arms are complete reaches the methods
# its arms name and no other (any other name raises NoMethodError), so the
# share walk follows the arms as calls and joins nothing else with what it
# does not see; `lookup`'s `env` stays apart from `shout`'s String (#9627).
class Greeter
  def hello(x) = "hello #{x}"
  def bye(x) = "bye #{x}"
end
g = Greeter.new
name = "#{ARGV.size > 0 ? 'bye' : 'hello'}".to_sym
puts g.send(name, "w")
puts g.public_send("#{name}", "v")

def shout(s) = s.upcase!
def lookup(env = ENV) = env["HOME"]
t = +"hi"
shout(t)
p t, lookup.nil?
