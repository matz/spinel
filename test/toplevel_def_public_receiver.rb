# A top-level def made public (`public :m`, `public def m`, a bare `public`
# ahead of the def) is an Object method, so a call with an explicit
# receiver runs it, whatever the receiver is: an Integer, a Float, a
# String, a Symbol, nil, true, false, an Array, a Hash, a Range, a class
# value, a program object. The def has no self of its own, so the receiver
# runs for its effect, then the call.

def plain = 1
def adds(a, b = 2, *rest, k: 3) = [a, b, rest, k]
def nothing = nil
def maybe(x) = x ? 1 : nil
def put_it(s) = $stdout.write(s, "\n")
def with_block(&b) = b ? b.call : :none
def yields(x) = block_given? ? yield(x) : :noblock
def twice = [yield, yield]

p((3.plain rescue $!.message))
p((nil.plain rescue $!.message))
public :plain, :adds, :nothing, :maybe, :put_it, :with_block, :yields, :twice
p 3.plain, 2.5.plain, "s".plain, :s.plain, nil.plain, true.plain, false.plain
p [1].plain, { a: 1 }.plain, (1..2).plain, Integer.plain, String.plain
class Foo; end
p Foo.new.plain, Foo.plain

# arguments, defaults, a rest, a keyword
p 3.adds(1), "x".adds(1, 5, 6, k: 7), nil.adds(9, 8)

# a value that can be nil, and one that is nil
p 1.maybe(true), 1.maybe(false), 7.nothing, 7.nothing.nil?
x = 4.maybe(false)
p x
8.put_it("printed")

# the receiver runs first, once, and for its effect
$log = []
def recv_log(tag) = ($log << tag; Foo.new)
def arg_log(tag) = ($log << tag; 6)
p recv_log(:recv).adds(arg_log(:arg))
p $log

# a block, a block parameter, a yield
p 3.with_block { :blk }, 3.with_block
p 3.yields(1) { |v| v + 10 }, 3.yields(2)
p nil.twice { 4 }
r = 9.yields(3) { |v| break v * 100 }
p r

# the version live at the call, not the table's last one
def ver = :first
public :ver
p 3.ver
def ver = :second
p((3.ver rescue $!.message))
public :ver
p 3.ver

# a def made public as it is defined, and one a bare word covers
public def pub_def = :pub_def
p 3.pub_def
public
def bare_pub = :bare
private
def back_to_private = :priv
p 3.bare_pub
p((3.back_to_private rescue $!.message))

# an alias takes the visibility of the def it names
def orig = :orig
public :orig
alias copy orig
p 3.copy

# a def still private raises, on each receiver
def hidden = 1
p((3.hidden rescue $!.message))
p((nil.hidden rescue $!.message))
p((true.hidden rescue $!.message))
p((Integer.hidden rescue $!.message))
p((Foo.new.hidden rescue $!.message))
