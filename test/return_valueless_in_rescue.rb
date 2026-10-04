# A return from a method or lambda with no value to give back -- a void
# method, a nil-valued lambda -- evaluates its value inside the
# begin..rescue it leaves, so a value that raises is rescued there; and a
# nil-valued lambda can return from inside one at all.
def void_return(n)
  begin
    return puts(1 / n)
  rescue ZeroDivisionError
    puts "rescued in method"
  end
  nil
end
void_return(0)

side = ->(fail) { raise "side" if fail; nil }
f = -> do
  begin
    return side.call(true)
  rescue
    puts "rescued in lambda"
  end
  nil
end
p f.call

def nil_value
  puts "nil_value ran"
  nil
end
g = -> do
  begin
    return nil_value
  rescue
  end
  nil
end
p g.call

h = -> do
  begin
    [1, 2].each { |v| return nil if v == 1 }
  rescue
  end
  nil
end
p h.call

# Each return pops the frames it leaves: a raise after the calls reaches
# the rescue around them.
begin
  void_return(1)
  f.call
  g.call
  h.call
  raise "after"
rescue => e
  p e.message
end
