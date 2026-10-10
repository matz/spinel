# A method that ends by calling a method which never returns a value, and
# otherwise returns early: the tail call is evaluated for effect. Whether it
# answers a value was decided from every method of that name in the program,
# so an unrelated `Other#finish` made Job#run return the call's void result,
# and the C did not compile.
class Job
  def finish
    Missing.call
  end

  def run(n)
    return [n, "early"] if n > 0
    finish
  end
end

class Other
  def finish
    1
  end
end

p Job.new.run(1)
p Other.new.finish
begin
  Job.new.run(0)
rescue NameError => e
  puts e.class
end
