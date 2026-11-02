# A constant written through its path, `Mod::X = Array.new(n) { Obj.new }`,
# builds at its object-table kind, as `X = ...` inside the module does
# (test/object_array_constant_array_new.rb). The path write emitted the call
# at its own poly type into the sp_PtrArray slot, and the C did not compile.

class Cb
  attr_reader :name
  def initialize(name) = (@name = name)
end

module Reg; end
Reg::NONE = Array.new(0) { Cb.new(nil) }
Reg::PAIR = Array.new(2) { |i| Cb.new(:"c#{i}") }

class Box; end
Box::ONE = Array.new(1) { Cb.new(:b) }

def first_name(list) = list.empty? ? nil : list[0].name

def names(list)
  out = []
  list.each { |cb| out << cb.name }
  out
end

p Reg::NONE.size, first_name(Reg::NONE)
p Reg::PAIR.size, first_name(Reg::PAIR), names(Reg::PAIR)
p names(Box::ONE)
