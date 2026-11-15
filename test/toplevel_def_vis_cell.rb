# A top-level def whose visibility only the run tells: a `public` word under
# a condition, in a loop, a case, an expression, a rescue clause, or a call
# in a method body that the load does not order against the word. The call
# with an explicit receiver reads the def's visibility cell: it raises the
# private-method error where the word has not run, and runs the def where it
# has. respond_to? reads the same cell.

class Foo; end
$yes = ARGV.empty?
$no = !$yes

def tell(v) = (v.m1 rescue $!.message)

def m1 = 1
def m2 = 2
def m3 = 3
def m4 = 4
def m5 = 5
def m6 = 6
def m7 = 7

public :m1 if $yes
public :m2 if $no
case $yes
when true then public :m3
else public :m4
end
$yes && public(:m5)
begin
  raise "x"
rescue
  public :m6
end
n = 0
while n < 2
  p((3.m7 rescue $!.message))
  public :m7 if n == 1
  n += 1
end

p 3.m1, nil.m1, true.m1, Integer.m1, Foo.new.m1
p((3.m2 rescue $!.message), (nil.m2 rescue $!.message), (Foo.new.m2 rescue $!.message))
p 3.m3, 3.m5, 3.m6
p((3.m4 rescue $!.message))
p 3.m7

# the receiver runs once, whichever arm answers
$log = []
def mk(tag) = ($log << tag; Foo.new)
def with_arg(a) = [:got, a]
def hidden_arg(a) = [:got, a]
public :with_arg if $yes
public :hidden_arg if $no
p mk(:r).with_arg(1)
p((mk(:s).hidden_arg(2) rescue $!.message))
p $log

# a boxed receiver
mixed = [1, "s", nil, :sym, Foo.new]
p mixed.map { |x| x.m1 }
p mixed.map { |x| (x.m2 rescue :private) }

# a call in a method body, before and after the word
def go(x) = (x.m8 rescue $!.message)
def m8 = 8
p go(nil)
public :m8 if $yes
p go(nil)
p go(3)

# respond_to? reads the cell
p 3.respond_to?(:m1), 3.respond_to?(:m2), 3.respond_to?(:m2, true), nil.respond_to?(:m1)
p mixed.map { |x| x.respond_to?(:m1) }.all?
p mixed.map { |x| x.respond_to?(:m2) }.any?

# a word the run takes back, and a later one that opens it again: a call in a
# method body and respond_to? (with include_all too) read the cell each time
def m88 = 8
public :m88
def go88 = (3.m88 rescue $!.class)
p go88, 3.respond_to?(:m88), 3.respond_to?(:m88, true)
private :m88 if ARGV.empty?
p go88, 3.respond_to?(:m88), 3.respond_to?(:m88, true)
public :m88 if ARGV.empty?
p go88, 3.respond_to?(:m88), 3.respond_to?(:m88, true)
