# A module mixed into one builtin class runs with that builtin value as
# self: its attr_* and its @ivars read and write the value's instance
# variables, as a reopening of the class does.

module IntAttrs
  attr_reader :tag
  attr_writer :wtag
  def ivar = @ivar
  def label = "int #{self} #{tag.inspect}"
end
class Integer; include IntAttrs; end
p 3.tag
p 3.ivar
p 3.label
p 3.respond_to?(:tag)
begin
  3.wtag = 1
rescue FrozenError => e
  p e.class
end

module StrAttrs
  attr_reader :sattr
  def sivar = @sivar
end
String.include(StrAttrs)
p "s".sattr
p "s".sivar

module ArrAttrs
  attr_accessor :note
  def count_up = (@count = (@count || 0) + 1)
end
class Array; include ArrAttrs; end
a = [1, 2]
p a.note
a.note = "two"
p a.note
p a.count_up
p a.count_up
p [3].note

module ObjAttrs
  attr_reader :oattr
  def oivar = @oivar
end
class Object; include ObjAttrs; end
class Holder
  def initialize = (@oattr = 4; @oivar = 5)
end
p Holder.new.oattr
p Holder.new.oivar
p Object.new.oattr
p 7.oivar

# module_function copies run with the module as self: its own ivar
module Fn
  def fget = @fget
  module_function :fget
end
class String; include Fn; end
Fn.instance_variable_set(:@fget, 9)
p Fn.fget

# a program class that only shares a builtin's name keeps the module's
# ivars as its own
module NsAttrs
  attr_accessor :nv
end
module Ns
  class Integer
    include NsAttrs
  end
end
o = Ns::Integer.new
o.nv = 8
p o.nv
