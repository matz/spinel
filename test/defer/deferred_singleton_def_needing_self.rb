# Compiled with --defer-refusals: a singleton method that reads self, defined
# on a value that is not one user-class instance (a String local), is
# refused where it is emitted, so the method defining it raises
# NotImplementedError when it runs and a program that never runs it
# (activerecord's Explain#exec_explain, `def str.inspect; self; end`)
# builds. Without the flag it is refused.
# spinel: defer-refusals: 2:start true after
def explain(sql)
  str = "plan: #{sql}"
  def str.inspect
    self
  end
  str
end

puts "start"
begin
  explain("x")
rescue NotImplementedError => e
  puts e.message.include?("singleton method that needs a self")
end
puts "after"
proc { "a" }.ruby2_keywords
puts "not reached"
