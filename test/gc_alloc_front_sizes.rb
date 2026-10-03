# A constructor allocates through the allocator's front for its object's size
# class, picked where it is compiled from the size of the object instead of
# read from the slab's table. One class fills each size class such a front
# takes, up to 256 bytes with the header (1, 3, 5 .. 25 fields), one sits
# just past a boundary (2) and one goes the ordinary way (26). Sixty of each
# are made in turn, so that every class has a run open, and read back after
# a collection: an object given a slot of a smaller class would have the
# next one written over it.

class F1
  def initialize(v)
    @f0 = v
  end

  def sum
    @f0
  end
end

class F2
  def initialize(v)
    @f0 = v; @f1 = v + 1
  end

  def sum
    @f0 + @f1
  end
end

class F3
  def initialize(v)
    @f0 = v; @f1 = v + 1; @f2 = v + 2
  end

  def sum
    @f0 + @f1 + @f2
  end
end

class F5
  def initialize(v)
    @f0 = v; @f1 = v + 1; @f2 = v + 2; @f3 = v + 3; @f4 = v + 4
  end

  def sum
    @f0 + @f1 + @f2 + @f3 + @f4
  end
end

class F7
  def initialize(v)
    @f0 = v; @f1 = v + 1; @f2 = v + 2; @f3 = v + 3; @f4 = v + 4; @f5 = v + 5
    @f6 = v + 6
  end

  def sum
    @f0 + @f1 + @f2 + @f3 + @f4 + @f5 + @f6
  end
end

class F9
  def initialize(v)
    @f0 = v; @f1 = v + 1; @f2 = v + 2; @f3 = v + 3; @f4 = v + 4; @f5 = v + 5
    @f6 = v + 6; @f7 = v + 7; @f8 = v + 8
  end

  def sum
    @f0 + @f1 + @f2 + @f3 + @f4 + @f5 + @f6 + @f7 + @f8
  end
end

class F11
  def initialize(v)
    @f0 = v; @f1 = v + 1; @f2 = v + 2; @f3 = v + 3; @f4 = v + 4; @f5 = v + 5
    @f6 = v + 6; @f7 = v + 7; @f8 = v + 8; @f9 = v + 9; @f10 = v + 10
  end

  def sum
    @f0 + @f1 + @f2 + @f3 + @f4 + @f5 + @f6 + @f7 + @f8 +
      @f9 + @f10
  end
end

class F13
  def initialize(v)
    @f0 = v; @f1 = v + 1; @f2 = v + 2; @f3 = v + 3; @f4 = v + 4; @f5 = v + 5
    @f6 = v + 6; @f7 = v + 7; @f8 = v + 8; @f9 = v + 9; @f10 = v + 10; @f11 = v + 11
    @f12 = v + 12
  end

  def sum
    @f0 + @f1 + @f2 + @f3 + @f4 + @f5 + @f6 + @f7 + @f8 +
      @f9 + @f10 + @f11 + @f12
  end
end

class F15
  def initialize(v)
    @f0 = v; @f1 = v + 1; @f2 = v + 2; @f3 = v + 3; @f4 = v + 4; @f5 = v + 5
    @f6 = v + 6; @f7 = v + 7; @f8 = v + 8; @f9 = v + 9; @f10 = v + 10; @f11 = v + 11
    @f12 = v + 12; @f13 = v + 13; @f14 = v + 14
  end

  def sum
    @f0 + @f1 + @f2 + @f3 + @f4 + @f5 + @f6 + @f7 + @f8 +
      @f9 + @f10 + @f11 + @f12 + @f13 + @f14
  end
end

class F17
  def initialize(v)
    @f0 = v; @f1 = v + 1; @f2 = v + 2; @f3 = v + 3; @f4 = v + 4; @f5 = v + 5
    @f6 = v + 6; @f7 = v + 7; @f8 = v + 8; @f9 = v + 9; @f10 = v + 10; @f11 = v + 11
    @f12 = v + 12; @f13 = v + 13; @f14 = v + 14; @f15 = v + 15; @f16 = v + 16
  end

  def sum
    @f0 + @f1 + @f2 + @f3 + @f4 + @f5 + @f6 + @f7 + @f8 +
      @f9 + @f10 + @f11 + @f12 + @f13 + @f14 + @f15 + @f16
  end
end

class F19
  def initialize(v)
    @f0 = v; @f1 = v + 1; @f2 = v + 2; @f3 = v + 3; @f4 = v + 4; @f5 = v + 5
    @f6 = v + 6; @f7 = v + 7; @f8 = v + 8; @f9 = v + 9; @f10 = v + 10; @f11 = v + 11
    @f12 = v + 12; @f13 = v + 13; @f14 = v + 14; @f15 = v + 15; @f16 = v + 16; @f17 = v + 17
    @f18 = v + 18
  end

  def sum
    @f0 + @f1 + @f2 + @f3 + @f4 + @f5 + @f6 + @f7 + @f8 +
      @f9 + @f10 + @f11 + @f12 + @f13 + @f14 + @f15 + @f16 + @f17 +
      @f18
  end
end

class F21
  def initialize(v)
    @f0 = v; @f1 = v + 1; @f2 = v + 2; @f3 = v + 3; @f4 = v + 4; @f5 = v + 5
    @f6 = v + 6; @f7 = v + 7; @f8 = v + 8; @f9 = v + 9; @f10 = v + 10; @f11 = v + 11
    @f12 = v + 12; @f13 = v + 13; @f14 = v + 14; @f15 = v + 15; @f16 = v + 16; @f17 = v + 17
    @f18 = v + 18; @f19 = v + 19; @f20 = v + 20
  end

  def sum
    @f0 + @f1 + @f2 + @f3 + @f4 + @f5 + @f6 + @f7 + @f8 +
      @f9 + @f10 + @f11 + @f12 + @f13 + @f14 + @f15 + @f16 + @f17 +
      @f18 + @f19 + @f20
  end
end

class F23
  def initialize(v)
    @f0 = v; @f1 = v + 1; @f2 = v + 2; @f3 = v + 3; @f4 = v + 4; @f5 = v + 5
    @f6 = v + 6; @f7 = v + 7; @f8 = v + 8; @f9 = v + 9; @f10 = v + 10; @f11 = v + 11
    @f12 = v + 12; @f13 = v + 13; @f14 = v + 14; @f15 = v + 15; @f16 = v + 16; @f17 = v + 17
    @f18 = v + 18; @f19 = v + 19; @f20 = v + 20; @f21 = v + 21; @f22 = v + 22
  end

  def sum
    @f0 + @f1 + @f2 + @f3 + @f4 + @f5 + @f6 + @f7 + @f8 +
      @f9 + @f10 + @f11 + @f12 + @f13 + @f14 + @f15 + @f16 + @f17 +
      @f18 + @f19 + @f20 + @f21 + @f22
  end
end

class F25
  def initialize(v)
    @f0 = v; @f1 = v + 1; @f2 = v + 2; @f3 = v + 3; @f4 = v + 4; @f5 = v + 5
    @f6 = v + 6; @f7 = v + 7; @f8 = v + 8; @f9 = v + 9; @f10 = v + 10; @f11 = v + 11
    @f12 = v + 12; @f13 = v + 13; @f14 = v + 14; @f15 = v + 15; @f16 = v + 16; @f17 = v + 17
    @f18 = v + 18; @f19 = v + 19; @f20 = v + 20; @f21 = v + 21; @f22 = v + 22; @f23 = v + 23
    @f24 = v + 24
  end

  def sum
    @f0 + @f1 + @f2 + @f3 + @f4 + @f5 + @f6 + @f7 + @f8 +
      @f9 + @f10 + @f11 + @f12 + @f13 + @f14 + @f15 + @f16 + @f17 +
      @f18 + @f19 + @f20 + @f21 + @f22 + @f23 + @f24
  end
end

class F26
  def initialize(v)
    @f0 = v; @f1 = v + 1; @f2 = v + 2; @f3 = v + 3; @f4 = v + 4; @f5 = v + 5
    @f6 = v + 6; @f7 = v + 7; @f8 = v + 8; @f9 = v + 9; @f10 = v + 10; @f11 = v + 11
    @f12 = v + 12; @f13 = v + 13; @f14 = v + 14; @f15 = v + 15; @f16 = v + 16; @f17 = v + 17
    @f18 = v + 18; @f19 = v + 19; @f20 = v + 20; @f21 = v + 21; @f22 = v + 22; @f23 = v + 23
    @f24 = v + 24; @f25 = v + 25
  end

  def sum
    @f0 + @f1 + @f2 + @f3 + @f4 + @f5 + @f6 + @f7 + @f8 +
      @f9 + @f10 + @f11 + @f12 + @f13 + @f14 + @f15 + @f16 + @f17 +
      @f18 + @f19 + @f20 + @f21 + @f22 + @f23 + @f24 + @f25
  end
end

x1 = []
x2 = []
x3 = []
x5 = []
x7 = []
x9 = []
x11 = []
x13 = []
x15 = []
x17 = []
x19 = []
x21 = []
x23 = []
x25 = []
x26 = []
60.times do |i|
  x1 << F1.new(i * 2)
  x2 << F2.new(i * 3)
  x3 << F3.new(i * 4)
  x5 << F5.new(i * 6)
  x7 << F7.new(i * 8)
  x9 << F9.new(i * 10)
  x11 << F11.new(i * 12)
  x13 << F13.new(i * 14)
  x15 << F15.new(i * 16)
  x17 << F17.new(i * 18)
  x19 << F19.new(i * 20)
  x21 << F21.new(i * 22)
  x23 << F23.new(i * 24)
  x25 << F25.new(i * 26)
  x26 << F26.new(i * 27)
end
GC.start
bad = []
x1.each_with_index { |o, i| bad << 1 if o.sum != 1 * i * 2 + 0 }
x2.each_with_index { |o, i| bad << 2 if o.sum != 2 * i * 3 + 1 }
x3.each_with_index { |o, i| bad << 3 if o.sum != 3 * i * 4 + 3 }
x5.each_with_index { |o, i| bad << 5 if o.sum != 5 * i * 6 + 10 }
x7.each_with_index { |o, i| bad << 7 if o.sum != 7 * i * 8 + 21 }
x9.each_with_index { |o, i| bad << 9 if o.sum != 9 * i * 10 + 36 }
x11.each_with_index { |o, i| bad << 11 if o.sum != 11 * i * 12 + 55 }
x13.each_with_index { |o, i| bad << 13 if o.sum != 13 * i * 14 + 78 }
x15.each_with_index { |o, i| bad << 15 if o.sum != 15 * i * 16 + 105 }
x17.each_with_index { |o, i| bad << 17 if o.sum != 17 * i * 18 + 136 }
x19.each_with_index { |o, i| bad << 19 if o.sum != 19 * i * 20 + 171 }
x21.each_with_index { |o, i| bad << 21 if o.sum != 21 * i * 22 + 210 }
x23.each_with_index { |o, i| bad << 23 if o.sum != 23 * i * 24 + 253 }
x25.each_with_index { |o, i| bad << 25 if o.sum != 25 * i * 26 + 300 }
x26.each_with_index { |o, i| bad << 26 if o.sum != 26 * i * 27 + 325 }
p bad.uniq

# A slot a dead object leaves behind still holds its words. Dense fills every
# word with an Integer; Sparse is the same size, and its constructor writes
# one field and leaves five references for later. They read nil in a slot
# Dense had, and the collector walks them.

class Dense
  def initialize(v)
    @a = v
    @b = v + 1
    @c = v + 2
    @d = v + 3
    @e = v + 4
    @f = v + 5
  end

  def sum
    @a + @b + @c + @d + @e + @f
  end
end

class Sparse
  attr_reader :a
  attr_accessor :s, :t, :u, :v, :w
  def initialize(n)
    @a = n
  end

  def unset?
    @s.nil? && @t.nil? && @u.nil? && @v.nil? && @w.nil?
  end
end

def churn(n)
  junk = []
  n.times { |i| junk << Dense.new(i + 7) }
  junk.map { |o| o.sum }.sum
end

dense = 0
unset = 0
held = []
6.times do
  dense += churn(500)
  GC.start
  3000.times do |i|
    o = Sparse.new(i)
    unset += 1 if o.unset?
    held << o if i % 750 == 0
  end
end
puts dense
puts unset
held.each do |o|
  o.s = [o.a]
  o.t = [o.a, 1]
  o.u = [o.a, o.a]
  o.v = [4]
  o.w = [1, 2, 3]
end
GC.start
puts held.map { |o| o.s[0] + o.t.length + o.u.length + o.v[0] + o.w.length }.sum
