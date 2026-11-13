# A statement in a begin body with rescue clauses may not have run once
# the begin is done: a raise the rescue takes skips the rest of the body.
# A def, an undef or a reopening's def there is no sure version after it,
# where a later statement of the same body has run it. The statements
# ahead of the first that may raise what the clauses catch run whenever
# the begin does: for a `rescue LoadError` list, the first that may
# raise a LoadError (a raise of one, a call of a method that does) or
# leave the body by a jump. That first one runs too, up to its own first
# statement that may raise.

class Object; def f = "Object#f"; end
def self.f = "main.f"
begin
  raise "x"
  class << self; undef_method :f; end
rescue
  p :rescued
end
p((f rescue $!.class))

class A; def g = 1; end
begin
  Integer("zz")
  class A; def g = 2; end
rescue ArgumentError
  p :argument_error
end
p A.new.g

def h = 1
begin
  raise "x"
  def h = 2
rescue
end
p h
p nil.send(:h)

# the body runs whole when nothing raises, and a later statement of the
# body sees what it ran
def k = 1
begin
  def k = 2
  p k
rescue
end
p k

# the first statement that may raise runs: an undef in it, which nothing
# ahead of it can skip, stops the call
def self.v = "main.v"
begin
  x = 1
  class << self; undef_method :v; end
rescue
end
p((v rescue $!.class))

# a def ahead of the first raise runs whenever the begin does
begin
  def m = :first
  raise "x"
  def m = :never
rescue
end
p m

# under `rescue LoadError` another raise leaves the begin: the outer
# rescue takes it, past the rest of the inner body
def r = "a"
begin
  begin
    def r = "b"
    Integer("zz")
    def r = "c"
  rescue LoadError
  end
rescue ArgumentError
end
p r

# a raise of a LoadError, of a class below it, from a method
class MyLoadError < LoadError; end
def boom = raise(MyLoadError, "x")
def t = "a"
begin
  boom
  def t = "b"
rescue LoadError
end
p t

# a break out of a block, a throw out of a catch
def u = "a"
1.tap do
  begin
    def u = "b"
    break
    def u = "c"
  rescue LoadError
  end
end
p u
def w = "a"
catch(:t) do
  begin
    def w = "b"
    throw :t
    def w = "c"
  rescue LoadError
  end
end
p w

# main's own to_s, undefined after a statement that may raise and does
# not: the undef may not have run, and main's builtin entry tells
begin
  Integer(ARGV.size == 9 ? "z" : "1")
  class << self; undef_method :to_s; end
rescue
end
p((to_s rescue $!.class))
p((self.to_s rescue $!.class))
p inspect
