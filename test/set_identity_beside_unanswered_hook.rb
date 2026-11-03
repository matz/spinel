# A Set takes any member in a program whose only compare_by_identity is
# Set's own, beside a method_added hook and a send of a computed name. The
# hook made the load record call `Set.method_added(:compare_by_identity)`
# after Set's own def of the method, and that Symbol counted as the program
# spelling the name: the send's compare_by_identity arm then counted as
# making a Set an identity one, and every String member was refused. No
# method of the program's answers that hook call, so the name reaches no
# call.
require "set"

class Base
  def self.method_added(name)
    puts "added #{name}"
    super
  end
end

class Kid < Base
  def hi = 1
end

def ask(obj, name) = obj.public_send(name)

s = Set.new
s << "a" << 1 << 2.0
p s.include?("a"), s.include?(1), s.size
p Kid.new.hi
x = ARGV.empty? ? [3, 1, 2] : "abc"
p ask(x, :size)
