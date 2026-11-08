# A bare push, append or unshift (no argument) on a value that is an Array or an
# instance of a program class defining the same name. The Array answers itself
# and a frozen one raises FrozenError, as with no such class in the program; the
# class's own method answers for its instances. Also the safe-navigation form,
# a class of another arity, a Queue, a Hash and a reader of the name.
class U
  def push(*a) = [:upush, a]
  def append(*a) = [:uappend, a]
  def unshift(*a) = [:uunshift, a]
end
def mk(f) = f ? [1, 2] : U.new
p mk(true).push
p mk(true).append
p mk(true).unshift
p mk(false).push
p mk(false).append
p mk(false).unshift
p mk(true).push(3)
p mk(false).push(3)

a = mk(true)
b = a.push
b << 3
p a
p a.equal?(b)
a.append
a.unshift
p a
p mk(true).push.push(4).size

fz = ->(f) { f ? [1, 2].freeze : U.new }
%i[push append unshift].each do |m|
  begin
    fz.(true).public_send(m)
    puts "no error"
  rescue FrozenError => e
    puts "#{m}: #{e.message}"
  end
end

def nav(k)
  case k
  when 0 then [1, 2]
  when 1 then U.new
  end
end
p nav(0)&.push
p nav(1)&.push
p nav(2)&.push
p nav(0)&.append

class W
  def push(x) = [:w, x]
end
def mkw(f) = f ? [1, 2] : W.new
p mkw(true).push
p mkw(false).push(1)
begin
  mkw(false).push
rescue ArgumentError => e
  puts e.message
end

def mix(k)
  case k
  when 0 then [1, 2]
  when 1 then {a: 1}
  when 2 then Queue.new
  else U.new
  end
end
p mix(0).push
begin
  mix(1).push
rescue NoMethodError => e
  puts e.message
end
begin
  mix(2).push
rescue ArgumentError => e
  puts e.message
end
p mix(3).push

class R
  attr_reader :push
  def initialize = @push = :field
end
def mkr(f) = f ? [1, 2] : R.new
p mkr(true).push
p mkr(false).push
