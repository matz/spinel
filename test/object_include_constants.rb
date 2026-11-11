# A module included into Object (`Object.include(M)`, `class Object; include
# M; end`) puts its constants on Object's ancestors, so a bare read finds them
# at the top level and from any class or method body, as with main's own
# `include`: a nested module's constant beside an outer one of the same leaf
# name, a flat module's beside another namespace's, a class's own constant
# ahead of the mixed-in one, and the last module included first.
module Units
  Scale = 10
  module Metric
    Scale = 1000
  end
end
module Plain
  Offset = 7
end
module Other
  Both = :other
end
module Tag
  Which = :a
end
module TagB
  Which = :b
end
module Lone
  Kind = :lone
end

Object.include(Units::Metric)
class Reader
  def scale = Scale
end
def scale_in_def = Scale
p Scale
p Reader.new.scale
p scale_in_def

Object.include(Plain)
class Shadow
  Offset = 1
  def offset = Offset
end
class NoShadow
  def offset = Offset
end
p Offset
p Shadow.new.offset
p NoShadow.new.offset

class Object
  include Lone
end
class InBody
  def kind = Kind
end
p Kind
p InBody.new.kind

# Main's include and Object's are one class: the later one leads.
include Tag
Object.include(TagB)
class Which1
  def which = Which
end
p Which
p Which1.new.which

Object.include(Other)
p Both
class Both1
  def both = Both
end
p Both1.new.both

# A bare read of a leaf two namespaces define finds the one the included
# module holds, for a class method call as for a constant.
module H
  class Leaf
    def self.v = :h
  end
end
module O
  class Leaf
    def self.v = :o
  end
end
Object.include(H)
class LeafReader
  def r = Leaf.v
end
p Leaf.v
p LeafReader.new.r

# A read in a method of the reopened Object starts at Object's own list: a
# constant the top level defines ahead of the modules it holds, and the
# later include (main's or Object's) first.
module Under
  Shade = :mixin
end
Object.include(Under)
Shade = :top
module Pa
  Vee = :a
end
module Pb
  Vee = :b
end
Object.include(Pb)
include Pa
class Object
  def shade = Shade
  def vee = Vee
end
p 1.shade
p 1.vee

# Ancestor order is the method search's: prepends first, and a module
# already among the ancestors stays where it was, for Object and for a class.
module Ta
  Q = :a
end
module Tb
  Q = :b
end
Object.include(Ta)
Object.include(Tb)
Object.include(Ta)
class Twice
  def q = Q
end
p Twice.new.q
module Cp
  Z = :p
end
module Cm
  Z = :m
end
class Ordered
  prepend Cp
  include Cm
  def z = Z
end
p Ordered.new.z
module Ca
  Y = :a
end
module Cb
  Y = :b
end
class Doubled
  include Ca
  include Cb
  include Ca
  def y = Y
end
p Doubled.new.y
