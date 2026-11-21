# A NameError the program raises with `raise NameError` records no receiver,
# and a KeyError raised that way no key, also when nothing rescues it: a
# reopened #message that asks for one in the report gets ArgumentError, as in
# CRuby, not nil. A message that is not a String but reads like the runtime's
# own ("undefined method 'foo' ...") gives no name or receiver either.
class NameError
  def message
    r = begin; receiver.inspect; rescue ArgumentError => e; e.message; end
    "receiver: #{r}, name: #{name.inspect}"
  end
end

class Text
  def to_s = "undefined local variable or method 'zz' for main"
end

class KeyError
  def message
    k = begin; key.inspect; rescue ArgumentError => e; e.message; end
    "key: #{k}"
  end
end

begin
  raise KeyError
rescue KeyError => e
  puts e.message
end

begin
  raise NameError, nil
rescue NameError => e
  puts e.message
end

begin
  raise NoMethodError, :"undefined method 'foo' for nil"
rescue NameError => e
  puts e.message
end

begin
  raise NameError, Text.new
rescue NameError => e
  puts e.message
end

puts "before"
raise NameError
