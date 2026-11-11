# A method only nil answers among the values a yield can hold (`=~` beside
# an Integer that may be nil) is decided once for the call, over the blocks
# of every site. Each site's splice has to emit that one decision: a site
# whose block gives another kind of value takes the boxed form the others
# take, and its own NoMethodError.

h = { "a" => 1 }

def kept_in_local
  x = (yield =~ /a/)
  p x
end

def kept_in_array
  t = [yield =~ /a/, 1]
  p t
end

def kept_in_ivar
  @m = (yield =~ /a/)
  p @m
end

kept_in_local { h["zz"] }
begin
  kept_in_local { true }
rescue NoMethodError => e
  puts e.class
end

kept_in_array { h["zz"] }
begin
  kept_in_array { true }
rescue NoMethodError => e
  puts e.class
end

kept_in_ivar { h["zz"] }
begin
  kept_in_ivar { false }
rescue NoMethodError => e
  puts e.class
end
