# `recv.attr ||= v` and `recv.attr &&= v` on a nil receiver raise
# NoMethodError for the reader, as CRuby does; `recv&.attr ||= v` on nil
# does nothing and answers nil.
class Acc
  attr_accessor :work, :list, :name, :n
  def initialize; @work = nil; @list = nil; @name = nil; @n = 0; end
end
class Box
  attr_accessor :acc
  def initialize(a) = @acc = a
  def bump
    @acc.work ||= 5
  rescue NoMethodError => e
    puts "bump #{e.message}"
  end
end
$calls = 0
def pick(n) = ($calls += 1; n.odd? ? Acc.new : nil)
def lookup(n) = n == 1 ? Struct.new(:work).new(true) : nil

s = lookup(2)
begin; s.work ||= 1; rescue NoMethodError => e; puts "struct or #{e.message}"; end
begin; s.work &&= 1; rescue NoMethodError => e; puts "struct and #{e.message}"; end
t = lookup(1); t.work &&= 2; p t.work

a = pick(2)
begin; a.work ||= 1; rescue NoMethodError => e; puts "or #{e.message}"; end
begin; a.work &&= 1; rescue NoMethodError => e; puts "and #{e.message}"; end
begin; x = (a.work ||= 1); p x; rescue NoMethodError => e; puts "or value #{e.message}"; end
begin; y = (pick(4).list &&= [1]); p y; rescue NoMethodError => e; puts "and value #{e.message}"; end
begin; a.n += 1; rescue NoMethodError => e; puts "op #{e.message}"; end
a&.work ||= 1
p(a&.work ||= 2)
a&.work &&= 4
p a

# the receiver runs once
$calls = 0
begin; pick(2).work ||= 1; rescue NoMethodError => e; puts "call #{e.message} #{$calls}"; end
b = pick(1); b.work ||= 3; p [b.work, $calls]
b.work &&= 8; p b.work
p(b.work ||= 9)
b&.work ||= 3; p b.work

Box.new(nil).bump
bx = Box.new(Acc.new); bx.bump; p bx.acc.work
h = { a: Acc.new }
begin; h[:b].name ||= "n"; rescue NoMethodError => e; puts "hash #{e.message}"; end
h[:a].name ||= "n"; p h[:a].name
[nil, Acc.new].each { |e| begin; e.work ||= 2; p e.work; rescue NoMethodError => err; puts "each #{err.message}"; end }
z = Acc.new; z.list ||= []; z.list << 1; p z.list
puts "after"
