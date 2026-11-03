module Base; def dbl = self * 2; end
module M2; def dbl = super + 1; end
class Integer; include Base; include M2; end
p 5.dbl

module ABase; def tag = "at"; end
module AM; def tag = "mt+" + super; end
class Array; include ABase; include AM; end
p [1, 2].tag

module HBase; def tag = "ht#{size}"; end
module HM; def tag = "hm+" + super; end
class Hash; include HBase; include HM; end
p({ a: 1 }.tag)

module SBase; def tag = "st:#{self}"; end
module SM; def tag = "sm+" + super; end
class String; include SBase; include SM; end
p "x".tag

module FBase; def half = self / 2; end
module FM; def half = super + 0.25; end
class Float; include FBase; include FM; end
p 3.0.half

module OBase; def hello = "base"; end
module OM; def hello = "m+" + super; end
class Object; include OBase; include OM; end
class Q; end
p Q.new.hello
p hello

module Gone; def g = super; end
module Over; def g = "over+" + super; end
class K; include Gone; include Over; end
begin; p K.new.g; rescue NoMethodError => e; p e.class; end

module NGone; def n = super; end
module NOver; def n = super + 1; end
class N; include NGone; include NOver; end
begin; p N.new.n; rescue NoMethodError => e; p e.class; end

class P; def g = super; end
class C < P; def g = "c+" + super; end
begin; p C.new.g; rescue NoMethodError => e; p e.class; end

class Q; def g = super; def h = super; end
class R < Q
  def g = ["r", super]
  def h = super || "dflt"
  def respond_to_missing?(n, p = false) = true
end
begin; p R.new.g; rescue NoMethodError => e; p e.class; end
begin; p R.new.h; rescue NoMethodError => e; p e.class; end
class S < Q
  def g = "s+" + super
  def respond_to_missing?(n, p = false) = true
end
begin; p S.new.g; rescue NoMethodError => e; p e.class; end
class U < Q
  def g = "u+" + super
  def method_missing(name, *args) = super
  def respond_to_missing?(name, priv = false) = name == :zz || super
end
begin; p U.new.g; rescue NoMethodError => e; p e.class; end
class W < Q; def g = super.to_s; end
begin; p W.new.g; rescue NoMethodError => e; p e.class; end
class W; def method_missing(name, *args) = "mm:#{name}"; end
class Branch; def g(x = false) = x ? super : nil; end
class BranchUser < Branch
  def g = ["a", super()]
  def method_missing(name, *args) = "mm:#{name}"
end
p BranchUser.new.g
class BranchJoin < Branch
  def g = "c+#{super()}"
  def method_missing(name, *args) = "mm:#{name}"
end
p BranchJoin.new.g
class Arm < Q; def g = ((true ? super : 1); 7); def h = (super rescue "resc"); end
begin; p Arm.new.g; rescue NoMethodError => e; p e.class; end
p Arm.new.h
class Arm; def method_missing(name, *args) = "mm:#{name}"; end
class Bang < Q; def g = !super; def h = (super == nil); end
begin; p Bang.new.g; rescue NoMethodError => e; p e.class; end
begin; p Bang.new.h; rescue NoMethodError => e; p e.class; end
class Bang; def method_missing(name, *args) = "mm:#{name}"; end
class Thrower; def g = throw(:t); end
class ThrowUser < Thrower
  def g = !super
  def method_missing(name, *args) = "mm:#{name}"
end
p(catch(:t) { ThrowUser.new.g; :none })
class Same < Q; def g; x = super.freeze; p x; y = [super.itself]; p y; 7; end; end
begin; p Same.new.g; rescue NoMethodError => e; p e.class; end
class Same; def method_missing(name, *args) = "mm:#{name}"; end
