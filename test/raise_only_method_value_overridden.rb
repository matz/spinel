# The base class's `version` always raises, and a subclass answers a String.
# The call on a boxed receiver then dispatches to either, and is typed from
# the subclass, so comparing and interpolating it keep their String arms;
# only an instance of the base class raises.

class Migration
  def version = raise(NotImplementedError, "subclass")
end

class Numbered < Migration
  def version = "7"
end

def has?(ms, v) = ms.any? { |m| m.version == v }
def lines(ms) = ms.map { |m| "v#{m.version}" }

p has?([Numbered.new], "7")
puts lines([Numbered.new])
begin
  has?([Migration.new], "7")
rescue NotImplementedError => e
  puts "base: #{e.message}"
end
