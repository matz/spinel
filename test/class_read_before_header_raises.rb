# A class or module named above its `class`/`module` statement is not
# there yet: the read raises NameError, as a constant read before its
# assignment does. Reads that run after the header (a method or a lambda
# made earlier but called later, a reopening) still find it.

begin
  puts Later.new.hi
rescue NameError => e
  puts "#{e.class.name}: #{e.message}"
end

begin
  p LaterMod
rescue NameError => e
  puts "#{e.class.name}: #{e.message}"
end

p defined?(Later)

def make_later = Later.new.hi
pr = -> { Later.new.hi }

class Early
  def other = Later.new.hi
end

class Later
  def hi = "hi"
end
module LaterMod; end

p defined?(Later)
puts Later.new.hi
puts make_later
puts pr.call
puts Early.new.other
p LaterMod

class Later
  def bye = "bye"
end
puts Later.new.bye
