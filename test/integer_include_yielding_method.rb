# A module's yielding method mixed into Integer runs with the call's block,
# with a block or without one: the receiver is the Integer, the block's
# writes reach the caller's locals, break and next leave it, and the
# module's later include wins over the earlier one.
module Twice
  def twice = (yield self; yield self + 100; :twice)
end

module Fold
  def fold(base, step = 2)
    yield base + step + self
  end

  def wrap(&b) = (b.call(self); :wrapped)

  def given = block_given? ? yield(self) : :none

  def guarded
    yield self
    :normal
  ensure
    puts "ensure"
  end
end

module Plus
  def plus_one = yield(self) + 1
end

class Range
  def ends_twice = (yield first; yield last; :range_done)
end

class Array
  def first_two = (yield self[0]; yield self[1]; :array_done)
end

class Hash
  def each_key_twice = (keys.each { |k| yield k }; :hash_done)
end

module Early
  def first_call = (yield self; :early)
end

module Late
  def first_call = (yield self; :late)
end

Integer.include(Twice)
class Integer
  include Fold
end
Integer.include(Plus)
Integer.include(Early)
Integer.include(Late)

p 3.twice { |v| p v }
total = 0
p 4.twice { |v| total += v }
p total
log = []
5.twice { |v| log << v * 2 }
p log

p 1.fold(10) { |v| v }
p 1.fold(10, 20) { |v| v + 1 }
p 2.wrap { |v| p [:wrap, v] }
p 7.given
p 7.given { |v| v + 1 }

p 1.twice { break :out }
p 2.twice { next 5 }
p 3.guarded { |v| p v }
p 4.guarded { break :b }

p 9.first_call { |v| p [:blk, v] }
p (2 + 3).twice { |v| v }
n = 6
p (n * 2).fold(0) { |v| v * 10 }

def run(v) = v.twice { |x| x + 1 }
p run(1)
def folded(v) = v.fold(1) { |x| x * 2 }
def plused(v) = v.plus_one { |x| x * 2 }
p plused(3)
p plused(1048576)
p folded(1)
p folded(41)
p 1.twice { |a| 2.twice { |b| a + b } }
p 1.send(:twice) { |v| p [:send, v] }
p 2.public_send(:fold, 3) { |v| v }
p 3.twice(&:succ)
l = ->(v) { v * 3 }
p 4.fold(1, &l)

def early
  1.twice { return :returned }
  :after
end
p early

mixed = [1, "s", 2.5, nil]
mixed.each { |v| p v.respond_to?(:twice) }
x = [1, nil].first
p x&.twice { |v| v }
y = [nil, 1].first
p y&.twice { |v| v }

# a break and a next in the block of a reopened Range's, Array's and Hash's
p (1..3).ends_twice { |v| break [:range, v] }
p (1..3).ends_twice { |v| next 0 }
p [5, 6].first_two { |v| break [:array, v] }
p [5, 6].first_two { |v| next 0 }
p({ a: 1 }.each_key_twice { |k| break [:hash, k] })
p({ a: 1 }.each_key_twice { |k| next 0 })
