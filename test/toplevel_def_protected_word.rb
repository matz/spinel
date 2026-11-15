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
