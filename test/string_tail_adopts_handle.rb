# frozen_string_literal: true
# A String a method, a block or a spliced method answers at its tail is the
# String itself, not a copy of its bytes, whatever made it: a built String
# is the caller's own, a slice is a String of its own, a frozen literal is
# the one frozen object, an ivar or a parameter is the String the holder
# keeps. The caller mutates what it was handed and prints the source.

class Box
  def initialize = @iv = +"iv"
  def built = +"b" + "uilt"
  def interp(n) = "n#{n}"
  def slice(s) = s[1, 2]
  def lit = "lit"
  def ivar = @iv
  def param(x) = x
  def cond(f, x) = f ? x : "no"
  def kase(k, x)
    case k
    when 0 then x
    when 1 then "one"
    else @iv
    end
  end
  def iv_src = @iv
  def inner(f) = yield(f)
  def twice = [yield, yield]
end

b = Box.new
src = +"abcdef"

r = b.built
r << "!"
puts r, r.frozen?

r = b.interp(3)
r << "!"
puts r, r.frozen?

r = b.slice(src)
r << "!"
puts r, src, r.frozen?

r = b.lit
puts r, r.frozen?, r.equal?(b.lit)

r = b.ivar
r << "!"
puts r, b.iv_src, r.equal?(b.iv_src)

r = b.param(src)
r << "?"
puts r, src, r.equal?(src)

r = b.cond(true, src)
r << "#"
puts r, src, r.equal?(src)
r = b.cond(false, src)
puts r, r.frozen?

[0, 1, 2].each do |k|
  r = b.kase(k, src)
  puts r, r.frozen?, r.equal?(src), r.equal?(b.iv_src)
end

# a spliced method answering what its block answers
x = +"blk"
r = b.inner(1) { x }
r << "+"
puts r, x, r.equal?(x)
r = b.inner(1) { "lit" }
puts r, r.frozen?
r = b.inner(1) { x[0, 2] }
r << "+"
puts r, x
r = b.inner(0) { |f| f == 0 ? x : "other" }
puts r.equal?(x)
r = b.inner(1) { |f| f == 0 ? x : +"made" }
r << "!"
puts r, x
a, c = b.twice { x }
puts a.equal?(c), a.equal?(x)

# next and break values of the same
r = [1, 2].map { |i| next x if i == 1; "n#{i}" }
puts r[0].equal?(x), r[1], r[1].frozen?
r = [1, 2].each { |i| break x[0, 1] if i == 2 }
r << "!"
puts r, x
r = [1, 2].each { |i| break "lit" if i == 2 }
puts r, r.frozen?
r = [1, 2].each { |i| break i.to_s if i == 2 }
r << "!"
puts r

# a nil tail beside a String arm
def maybe(f, x) = f ? x : nil
r = maybe(true, src)
puts r.equal?(src)
puts maybe(false, src).inspect

# a begin/rescue tail
def guarded(x)
  Integer("z")
rescue ArgumentError => e
  e.message
end
r = guarded(1)
puts r.frozen?, r.equal?(guarded(1))
