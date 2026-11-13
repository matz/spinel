# `return v if v.is_a?(K)` answers v when K is a builtin the program reopens,
# or a module, as CRuby does. The narrowing of that return to K (#3259) is for
# a plain user class: a String came back nil, an Array, StandardError or module
# return did not build in C, and a Time return raised TypeError.
class String
  def shout
    upcase + "!"
  end
end

class Array
  def second
    self[1]
  end
end

class Time
  def stamp
    year
  end
end

class StandardError
  def tag
    "std"
  end
end

module Greet
  def hi
    "hi " + name
  end
end

class Ann
  include Greet

  def name
    "ann"
  end
end

class Bob
  include Greet

  def name
    "bob"
  end
end

def pick_s(x)
  return x if x.is_a?(String)
  raise ArgumentError, "not a String"
end

def pick_exact_s(x)
  return x if x.instance_of?(String)
  raise ArgumentError, "not exactly a String"
end

def pick_a(x)
  return x if x.is_a?(Array)
  raise ArgumentError, "not an Array"
end

def pick_t(x)
  return x if x.is_a?(Time)
  raise ArgumentError, "not a Time"
end

def pick_e(x)
  return x if x.is_a?(StandardError)
  raise ArgumentError, "not an error"
end

def pick_g(x)
  return x if x.is_a?(Greet)
  raise ArgumentError, "not a Greet"
end

def show_error
  yield
rescue ArgumentError => e
  p e.message
end

p pick_s("a")
p pick_s("b").shout
show_error { pick_s(:c) }
p pick_exact_s("d").shout
show_error { pick_exact_s(:e) }
p pick_a([1, 2])
p pick_a([3, 4]).second
show_error { pick_a(5) }
p pick_t(Time.at(0).utc).stamp
show_error { pick_t(5) }
p pick_e(KeyError.new("k")).tag
show_error { pick_e(5) }
p pick_g(Ann.new).hi
p pick_g(Bob.new).hi
show_error { pick_g(5) }
