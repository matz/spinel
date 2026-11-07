# A builtin reopening's method reads its own constant when a write of the
# builtin's may run: after the def, under a condition, in a later body.
LIM = 1
class Integer
  def a = LIM
  LIM = 3
end
p 5.a

LIM2 = 0
class Integer
  LIM2 = 4 if ARGV.size < 5
  def b = LIM2
end
p 5.b

LIM3 = 7
class Integer
  LIM3 = 4 if ARGV.size > 5
  def c = LIM3
end
p 5.c

LIM4 = 2
class Integer
  def d = LIM4
end
p 5.d
class Integer; LIM4 = 9; end
p 5.d

LIM5 = 10
class Integer
  LIM5 ||= 11
  def e = LIM5
end
p 5.e

LIM6 = 20
class Integer
  def f = [1, 2].map { |x| x + LIM6 }
end
p 5.f
Integer::LIM6 = 30
p 5.f

LIM7 = "top"
class Integer
  begin
    LIM7 = "own"
  rescue
  end
  def g = "#{LIM7}!"
end
p 5.g

LIM8 = 1.5
class Float
  def h = LIM8
end
p 2.0.h
class Float; LIM8 = 2.5; end
p 2.0.h

LIM9 = :top
module Kernel
  def kk = LIM9
end
p kk
module Kernel; LIM9 = :kernel; end
p kk

LIM10 = 100
class Integer
  def i = LIM10
  p 5.i
  LIM10 = 200
  p 5.i
end

ZZ = :top
module Comparable
  def zz = ZZ
end
p 3.zz
module Comparable; ZZ = :cmp; end
p 3.zz

PP = 1
class Proc
  def pp_ = PP
end
pr = proc { }
p pr.pp_
class Proc; PP = 2; end
p pr.pp_

MM = 1
class Integer
  def mm = MM
  MM = 5 unless ARGV.empty?
end
p 5.mm


DD = 7
class Integer
  def dd = defined?(DD)
  DD = 1 if ARGV.size > 5
end
p 5.dd

PL = 3
class Integer
  def pl
    case self
    in PL then :three
    else :other
    end
  end
  PL = 9 if ARGV.size > 5
end
p 3.pl
p 4.pl
