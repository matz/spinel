# A parameter with a non-empty default of objects (`= [Info.new]`,
# `= Array.new(n) { Info.new }`) that every caller passes an Array of that
# class: narrow_object_arrays narrows it to the object array, and its default
# is one more of its sources, built at that kind. The default was left out:
# infer_default_param_types joined its poly array back in every round, and
# the fixpoint ran to its round cap ("type inference did not converge";
# infer-test checks the round count). A call that leaves the argument out
# builds the default.
class Info
  attr_reader :name

  def initialize(name = "d")
    @name = name
  end
end

def emit(infos = [Info.new])
  infos.size
end

puts emit([Info.new])
puts emit([Info.new, Info.new])

def names(infos = Array.new(1) { Info.new("g") })
  infos.map(&:name).join(",")
end

puts names([Info.new("a"), Info.new("b")])

def collect(n, infos = [Info.new("s")])
  n.times { |i| infos << Info.new("c#{i}") }
  infos.map(&:name).join(",")
end

puts collect(2, [Info.new("p")])

def both(infos = [Info.new("b")])
  infos.map(&:name).join(",")
end

puts both([Info.new("x"), Info.new("y")])
puts both

def tagged(prefix, infos: [Info.new("k")])
  infos.map { |i| "#{prefix}#{i.name}" }.join(" ")
end

puts tagged("-", infos: [Info.new("x"), Info.new("y")])
puts tagged("+")
