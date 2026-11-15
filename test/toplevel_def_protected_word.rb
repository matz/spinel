# main has no `protected` (it is a Module's method): the word raises
# NoMethodError where it runs, and the def it wraps is made first.
def token = 1
begin
  protected :token
rescue NoMethodError => e
  puts "raised #{e.class}"
end
begin
  protected def tok2 = 2
rescue NoMethodError => e
  puts "raised #{e.class}"
end
p tok2
p 3.respond_to?(:token)
class K
  def b = 2
  protected :b
  def c = 3
end
p K.new.respond_to?(:b), K.new.respond_to?(:c)

# written bare, with no argument, it is a read of a name main does not have
begin
  protected
rescue NameError => e
  puts "raised #{e.class}"
end

# with parentheses and no argument it is a call: NoMethodError
begin
  protected()
rescue NoMethodError => e
  puts "raised #{e.class}"
end

# a public def before the rescued word and a plain def after it stay public
def token2 = 1
public def before_rescue = 4
begin
  protected :token2
rescue NoMethodError => e
  puts "raised #{e.class}"
end
def after_rescue = 5
p 3.respond_to?(:before_rescue), 3.respond_to?(:after_rescue)

# its arguments run first, in order, before the word raises: a visibility call
# among them takes effect, so does a def, a print and a write
def vis = 1
begin
  protected(public(:vis))
rescue NoMethodError => e
  puts "raised #{e.class}"
end
p 3.vis
begin
  protected(puts("argument"))
rescue NoMethodError => e
  puts "raised #{e.class}"
end
begin
  protected(raise("arg"))
rescue => e
  p e.class, e.message
end
$f = nil
begin
  protected($f = 1)
rescue NoMethodError
end
p $f
begin
  protected(public(:vis), def made = 3)
rescue NoMethodError => e
  puts "raised #{e.class}"
end
p made

# the word nested in the operands of another call raises there, whatever it
# is nested in: the operands before it run, the outer call never does (a
# private it would apply is not applied), and a visibility call among them
# takes effect
def nest = 1
def pass_through(x) = x
begin
  private(protected(public(:nest)))
rescue NoMethodError => e
  puts "raised #{e.class}"
end
p 3.nest
begin
  private(private(protected(public(:nest))))
rescue NoMethodError => e
  puts "raised #{e.class}"
end
p 3.nest
begin
  public(private(protected(public(:nest))), :nest)
rescue NoMethodError => e
  puts "raised #{e.class}"
end
p 3.nest
begin
  private(pass_through(protected(public(:nest))))
rescue NoMethodError => e
  puts "raised #{e.class}"
end
p 3.nest
begin
  private(puts("before"), protected(public(:nest)))
rescue NoMethodError => e
  puts "raised #{e.class}"
end
p 3.nest
begin
  p(protected(public(:nest)))
rescue NoMethodError => e
  puts "raised #{e.class}"
end
p 3.nest
begin
  x = [protected(public(:nest))]
rescue NoMethodError => e
  puts "raised #{e.class}"
end
p 3.nest
begin
  private(protected(public(:nest)))
rescue NoMethodError => e
  puts "raised #{e.class}"
end
p 3.nest
begin
  private protected(public(:nest), def nested_made = 4)
rescue NoMethodError => e
  puts "raised #{e.class}"
end
p nested_made, 3.nest
