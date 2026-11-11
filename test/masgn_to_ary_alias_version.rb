# A multiple assignment takes an object whole until a to_ary exists, and
# converts it through an alias made later: the alias is a version of
# to_ary like a def.

class Duo
  def pair = [1, 2]
end

o = Duo.new
a, b = o
p [a.class, b]

class Duo
  alias to_ary pair
end
a, b = o
p [a, b, o.to_ary]
