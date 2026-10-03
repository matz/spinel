# A statement Array push runs its receiver before its arguments
# `recv << v`, `recv.push(v)` and `recv.append(v)` whose value goes nowhere
# were written as one C call with the receiver and the value as sibling
# arguments, which gcc evaluates right to left: `a << t(1) << t(2)` ran
# t(2) first, and a receiver its argument reassigns took the push on the
# new Array. CRuby runs the receiver, then each argument, once each.
class Cell
  attr_reader :v
  def initialize(v) = @v = v
end

def t(n) = (puts "t#{n}"; n)
def s(n) = (puts "s#{n}"; "s#{n}")
def f(n) = (puts "f#{n}"; n + 0.5)
def mk(n) = (puts "mk#{n}"; Cell.new(n))
def ia(a) = (puts "ia"; a)
def sa(a) = (puts "sa"; a)
def fa(a) = (puts "fa"; a)
def pa(a) = (puts "pa"; a)

puts "-- Integer Array"
a = [0]
a << t(1) << t(2)
ia(a) << t(3)
ia(a) << t(4) << t(5)
ia(a).push(t(6))
ia(a).append(t(7))
a.push(t(8)) << t(9)
p a

puts "-- String Array"
b = ["x"]
b << s(1) << s(2)
sa(b) << s(3)
sa(b).push(s(4))
sa(b).append(s(5))
p b

puts "-- Float Array"
g = [0.5]
g << f(1) << f(2)
fa(g) << f(3)
fa(g).push(f(4))
p g

puts "-- Array of anything"
h = [0, "x"]
h << t(1) << s(2)
pa(h) << s(3)
pa(h).push(t(4))
pa(h).append(s(5))
h << mk(6) << mk(7)
p h.size, h[7].v, h[8].v

puts "-- a closure that advances"
toks = ["a", "b", "c"]
i = 0
nxt = -> { i += 1; toks[i - 1] }
out = []
out << nxt.call << nxt.call << nxt.call
p out

puts "-- several arguments"
m = [0]
ia(m).push(1, 2)
ia(m).push(t(3), t(4))
m.push(m.size, m.size)
p m

puts "-- a receiver the argument reassigns"
v = [1]
v0 = v
v << (v = [5]; 6)
p v, v0
w = ["a"]
w0 = w
w.push((w = ["z"]; "b"))
p w, w0
c = [1]
c0 = c
la = -> { c = [100]; 5 }
c << la.call
p c, c0

class Bank
  def initialize
    @a = [1]
    @p = [1, "x"]
  end

  def swap
    @a = [100]
    5
  end

  def swapp
    @p = [nil, 2]
    :sym
  end

  def run
    old = @a
    @a << swap
    p old, @a
    oldp = @p
    @p.push(swapp)
    p oldp, @p
    @a.push(@a.size, @a.size)
    p @a
  end
end
Bank.new.run

$g = [1]
def gswap = ($g = [7]; 3)
g0 = $g
$g << gswap
p g0, $g

puts "-- an ivar a call on another object reassigns"
# The push keeps its one C call when nothing but a constructor assigns the
# receiver's ivar (@tape). Each of the others is assigned some other way,
# which the call reaches through the object it is made on.
module Wiper
  def wipe = @lane = [500]
end

class Hand
  def initialize(owner) = @owner = owner
  def rewind = (@owner.rewind; 2)
  def wipe = (@owner.wipe; 3)
  def set = (@owner.drum = [300]; 4)
  def within = (@owner.instance_eval { @vat = [400] }; 5)
  def idle = 6
end

class Reel
  include Wiper
  attr_writer :drum

  def initialize
    @reel = [1]
    @spool = [1]
    @cut = -> { @spool = [200]; 1 }
    @lane = [1]
    @drum = [1]
    @vat = [1]
    @tape = [1]
    @hand = Hand.new(self)
    @pail = [1]
    old = @pail
    @pail << (@pail = [600]; 7)
    p old, @pail
  end

  def rewind = @reel = [100]

  def run
    old = [@reel, @spool, @lane, @drum, @vat, @tape]
    @reel << @hand.rewind
    @spool << @cut.call
    @lane.push(@hand.wipe)
    @drum << @hand.set
    @vat.append(@hand.within)
    @tape << @hand.idle
    p old
    p [@reel, @spool, @lane, @drum, @vat, @tape]
  end
end
Reel.new.run

puts "-- an ivar a method assigns, pushed a number"
# Math's functions and a clock read run no code of the program's, so the
# push keeps its one C call (@laps, @ticks). A class method does, and so
# do arithmetic and a comparison on an object, through its coerce and its
# ==, and a yield.
class Deck
  def self.owner=(o)
    @owner = o
  end
  def self.cut = (@owner.recut; 2.0)
end

class Turn
  def initialize(owner) = @owner = owner
  def coerce(n) = (@owner.resum; [n, 9.0])
  def ==(n) = (@owner.reflag; true)
end

class Lap
  def initialize = start

  def start
    @laps = [1.0]
    @ticks = [1.0]
    @cuts = [1.0]
    @sums = [1.0]
    @fed = [1.0]
    @flags = [false]
    @n = 16.0
    @turn = Turn.new(self)
    Deck.owner = self
  end

  def recut = @cuts = [100.0]
  def resum = @sums = [200.0]
  def reflag = @flags = [nil]
  def feed = @fed << yield

  def run
    old = [@laps, @cuts, @sums, @fed, @flags]
    @laps << Math.sqrt(@n) * 2.0
    @ticks << Process.clock_gettime(Process::CLOCK_MONOTONIC)
    @cuts << Deck.cut
    @sums << Math.sqrt(4.0) + @turn
    feed { @fed = [300.0]; 3.0 }
    @flags << (Math.sqrt(4.0) == @turn)
    p old
    p [@laps, @cuts, @sums, @fed, @flags]
    p @ticks.size, @ticks[1] > 0.0
  end
end
Lap.new.run

puts "-- fresh objects, each held while the next is built"
total = 0
k = 0
while k < 600
  live = []
  live << Cell.new(k) << Cell.new(-k) << Cell.new(1)
  total += live[0].v * 2 + live[1].v + live[2].v
  k += 1
end
p total
