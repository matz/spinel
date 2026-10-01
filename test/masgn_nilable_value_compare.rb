# A multiple assignment's target takes the value the right side supplies
# it, and that value can be the nil sentinel of an Integer or Float slot as
# a plain write's can. Only the targets the right side could not supply
# were marked as holding it, so `a, b = z, 0` with `z` an Integer that may
# be nil compared `a` as a number: `a >= 0` answered false where CRuby
# raises NoMethodError. Through the first and a later target, a swap, a
# helper written in a block, and a scalar right side.
def first(z)
  a, b = z, 0
  p [a, b]
  p(a >= 0)
rescue NoMethodError => e
  p e.class
end
first(nil)
first(2)

def second(z)
  a, b = 0.5, z
  p(b < 1.0)
rescue NoMethodError => e
  p e.class
end
second(nil)
second(1.5)

def swap(z)
  a = 1
  b = z
  a, b = b, a
  p(a > 0)
rescue NoMethodError => e
  p e.class
end
swap(nil)
swap(3)

def in_block(x, z)
  if x
    [0].each do |_q|
      hb, hm = z, 0
      x = hb
    end
    p(x >= 0)
  end
rescue NoMethodError => e
  p e.class
end
in_block(1.5, nil)
in_block(1.5, 2.5)

def scalar(z)
  a, b = z
  p [a, b]
  p(a <= 9)
rescue NoMethodError => e
  p e.class
end
scalar(nil)
scalar(4)
