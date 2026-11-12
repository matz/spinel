# rfind on a receiver known only at run time, two shapes the poly arm of
# builtins/enumerable.rb's rfind has to keep:
#   - a safe navigation: nil answers nil, an Array its last match, and a class
#     with its own rfind that class's answer;
#   - an Array subclass: one that defines rfind answers with its own, one that
#     does not inherits Array's.
class Shelf
  def rfind; :shelf; end
end

class Own < Array
  def rfind; :own; end
end

class Plain < Array
end

def sn(x) = x&.rfind { |v| v != 3 }
def ask(x) = x.rfind { |v| v != 3 }

p sn([3, 1, 2])
p sn(["b", "a"])
p sn(nil)
p sn(Shelf.new)
p sn(Own.new)
p sn(Plain.new.push(1, 3))

p ask([3, 1, 2])
p ask(Own.new)
p ask(Shelf.new)
p ask(Plain.new.push(5, 4, 3))
p ask(Own.new.push(1))

def raises(label)
  r = begin
    yield
  rescue NoMethodError
    "NoMethodError"
  end
  puts "#{label}: #{r.inspect}"
end
raises("safe-nav hash") { sn({ a: 1 }) }
raises("hash") { ask({ a: 1 }) }
