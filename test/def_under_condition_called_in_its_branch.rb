# A def under a run-time condition is there for the calls after it in its
# own branch, and in a method called from there; a call where no branch
# defined it raises NoMethodError.
flag = [1].size == 1

if flag
  def in_branch(x)
    x + 1
  end
  p in_branch(1)
end

def through_method
  later(2)
end

if flag
  def later(x)
    x * 3
  end
  p through_method
end

if [1].size == 2
  def never_made
    1
  end
end
begin
  never_made()
rescue NoMethodError
  puts "never_made raises"
end

# Before a branch defines it, a call still reaches method_missing.
class Box
  def method_missing(name, *args)
    "missing #{name}"
  end
end
box = Box.new
p box.greet
if flag
  class Box
    def greet
      "hi"
    end
  end
end
p box.greet
