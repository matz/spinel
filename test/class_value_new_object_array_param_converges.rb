# A parameter of an initialize that every static construction gives an Array
# of one class (`UA.new([Tok.new], "a")`), in a program with a `new` on a
# Class value that may reach the class (`error_type.new("code", 3)`, `k.new`
# on an element of KINDS): the binding gives the call's arguments to the
# parameter too. narrow_object_arrays did not see the call and narrowed the
# parameter to the object array, the binding joined the String into it, the
# write pass put the pin back, and the fixpoint ran to its round cap ("type
# inference did not converge"; infer-test checks the round count). The
# parameter stays boxed, and a `new` on the Class value builds the object
# with the Array it is given.
class Tok
  attr_reader :name

  def initialize(name = "t")
    @name = name
  end
end

class UA
  def initialize(tokens, kind)
    @tokens = tokens
    @kind = kind
  end

  def kind
    @kind
  end

  def names
    @tokens.map { |t| t.name }.join(",")
  end
end

class Other
  def initialize(label, n)
    @label = label
    @n = n
  end

  def names
    "#{@label}#{@n}"
  end
end

class Err < StandardError
  def initialize(msg, res = nil)
    super(msg)
    @res = res
  end

  def res
    @res
  end
end

def error_type
  Err
end

KINDS = [UA, Other]
puts KINDS.size

u = UA.new([Tok.new, Tok.new("b")], "a")
puts u.kind
puts u.names

begin
  raise error_type.new("code", 3)
rescue Err => e
  puts e.message
  puts e.res
end

k = KINDS[0]
v = k.new([Tok.new("x"), Tok.new("y")], "c")
puts v.names
puts KINDS[1].new("o", 2).names
