# A top-level def is a private method of Object; a def of the same name in a
# reopening of Object that runs under a condition, or again in a loop, is a
# public one. Which of them a call with an explicit receiver reaches, and
# whether it raises for a private one, is known only at run time: each
# arm of the method's version entry runs its version, or raises for the
# private one with the receiver, the name and the arguments of the call.

def report(e)
  puts "#{e.class}: #{e.message}"
  p [e.name, e.receiver, e.args]
end

# the public def of Object runs: its version is the live one
def on(a, b); [:main, a, b]; end
if ARGV.size == 0
  class Object
    def on(a, b); [:obj, a, b]; end
  end
end
p nil.on(1, 2)
p 3.on(4, 5)
p "s".on(6, 7)

# it does not run: the top-level def is private
def off(a, b); [:main, a, b]; end
if ARGV.size > 0
  class Object
    def off(a, b); [:obj, a, b]; end
  end
end
[nil, 1, 2.5, "s", :sym, [1]].each do |v|
  begin
    p v.off(8, 9)
  rescue NoMethodError => e
    report(e)
  end
end

# the def is made by the second pass of a loop: the first pass is refused
def lp; 1; end
2.times do
  begin
    p nil.lp
  rescue NoMethodError => e
    report(e)
  end
  begin
    p 7.lp
  rescue NoMethodError => e
    report(e)
  end
  class Object
    def lp; 2; end
  end
end

# public_send refuses a private method of either, and sends it through send
def ps; 1; end
2.times do
  begin
    p nil.public_send(:ps)
  rescue NoMethodError => e
    report(e)
  end
  class Object
    def ps; 2; end
  end
end

# a call with a block stays closed on the top-level def's version
def blk(a); yield(a) + 1; end
if ARGV.size > 0
  class Object
    def blk(a); yield(a) * 10; end
  end
end
begin
  p nil.blk(2) { |x| x + 1 }
rescue NoMethodError => e
  report(e)
end

# splatted and keyword arguments are the list #args holds
def sp(*a, **k); [:main, a, k]; end
if ARGV.size > 0
  class Object
    def sp(*a, **k); [:obj, a, k]; end
  end
end
x = [9, 10]
[-> { nil.sp(1, 2) }, -> { nil.sp(*[3, 4]) }, -> { nil.sp(5, k: 6) }, -> { 2.5.sp(*x, 11) }, -> { 1.sp }].each do |f|
  begin
    f.call
  rescue NoMethodError => e
    report(e)
  end
end

# a public_send of a name known only at run time takes the same arms: its
# arm for each name is made after the entries were, and asks for the
# entry of its face then
def dn = 1
def dgo(n) = (nil.public_send(n) rescue :private)
nm = [:dn].first
2.times do
  p dgo(:dn)
  p((2.public_send(nm) rescue :private))
  class Object
    def dn = 2
  end
end
