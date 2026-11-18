# spinel: int64
# The extent of each body (--dump-load-record's extent lines): where it may
# start (from), the statement after which it has certainly run (sure), the
# most runs it has (count) and whether it runs at a time the syntax does not
# tell (kept). A method nothing calls never runs; one called once at the top
# level starts and is sure at that call; one called in a loop or under a
# rescue runs many times and is sure only where the call is. A block of
# 0.times runs no time, of 1.times and of a one-element literal's each once,
# of 3.times many; a stored lambda runs when the program calls it, an at_exit
# body after the load phase, and a file required in a method, which no load
# point places, from the statement that calls the method.
# A call that runs only under a condition inside its statement (`c && K.a`, a
# ternary, a modifier if, a case arm) is not sure at it, nor is a block of one
# (`c && 1.times {}`); an iterator that may skip its block (uniq, sort_by!,
# slice_before) is not sure on a literal, one that runs it once per element is;
# a count past 2^32 is not a count of none or once. An at_exit inside a block
# that runs no time runs no time. Nor is a call under `&.`, `defined?`, an
# op-write or a begin with a raise before it, or a block of a `&.` call.
def load_it = require_relative("record_extents_lib")
class K
  def self.never = 1
  def self.once = 2
  def self.loop_run = 3
  def self.guarded = 4
end
K.once
load_it
3.times { K.loop_run }
begin
  K.guarded
rescue
  p :rescued
end
0.times { p :zero }
1.times { p :one }
[7].each { |x| p x }
l = -> { 5 }
at_exit { p l.call }
p K.loop_run + K.guarded
c = ARGV.size > 5
class G
  def self.a = 1
  def self.b = 2
  def self.c = 3
  def self.d = 4
end
x = c && G.a
p(c ? G.b : 0)
y = (G.c if c)
z = case c when true then G.d else 0 end
w = c && 1.times { p :cond }
v = c ? [1].each { p :tern } : 0
[1].each_with_index { |e, i| p e }
[1].uniq { |e| e }
[1].sort_by! { |e| e }
[3, 4].slice_before { |e| e > 3 }
4294967296.times { break } if c
4294967297.times { break }
0.times { at_exit { p :never } }
p x, y, z, w, v
class H
  def self.a = 1
  def self.b = 2
  def self.c = 3
  def self.d = 4
  def self.e = 5
  def self.f = 6
end
class NN
  def foo(v) = v
end
class OA
  attr_accessor :x
end
nn = c ? NN.new : nil
nn&.foo(H.a)
nav1 = nn&.tap { p :nav1 }
nav2 = [nn&.then { p :nav2 }, 1]
dfn = defined?(H.b)
hh = [1]
hh[0] ||= H.c
beg = begin
  raise "q" unless c
  H.d
rescue
  0
end
oa = OA.new
oa.x = 5
oa.x ||= H.e
hk = { k: 1 }
hk[:k] &&= H.f
p nav1, nav2, dfn, hh, beg, oa.x, hk
