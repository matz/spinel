# A call of a method CRuby answers, that no route of the compiler takes, in
# code that does not run in this program (a branch not taken, a lambda not
# called, a rescue that is not reached, a method nothing calls) leaves the
# program's answer alone: the call raises only where it runs. The names that
# CRuby does not answer that way keep raising what CRuby raises: a private
# method called with a receiver, a name the receiver's class does not have,
# a method the Kernel does not give a BasicObject. A method whose answer
# turns on the name it is given raises a NameError, as CRuby does for a name
# that is not allowed, and a block argument that is no proc raises its
# TypeError.
def never(flag)
  return 1 unless flag
  global_variables
end

def uncalled
  local_variables
end

class K
  def one = flag? ? private_methods : 1
  def flag? = false
  def self.cm = false ? class_variables : 2
end

counted = 0
counted += 1 if false && local_variables
counted += never(false)
counted += K.new.one
counted += K.cm
fn = -> { Kernel.global_variables }
counted += 1 if [].size > 3 && 5.singleton_methods
counted += 1 if [].size > 3 && String.instance_methods(false).size > 0
begin
  counted += 10
rescue NameError
  counted += local_variables.size
end
p counted

def show
  yield
rescue NoMethodError, NameError => e
  puts "#{e.class}: #{e.message}"
end

show { Object.new.puts "x" }
show { 5.local_variables }
show { Integer.global_variables }
show { Comparable.global_variables }
show { fooo }
show { Object.new.zork }
show { String.zork }
show { Object.new.instance_methods }
show { 5.instance_method(:+) }

class B < BasicObject
  def try = local_variables
end
show { B.new.try }

def name_error
  yield
rescue NameError => e
  puts "NameError: #{e.is_a?(NameError)}"
end
nm = "bad"
name_error { Object.class_variable_get(nm) }
name_error { Object.class_variable_set(nm, 1) }
name_error { Object.class_variable_defined?("bad") }
name_error { Object.const_source_location(nm) }
name_error { Object.const_set(nm, 1) }
name_error { Object.new.remove_instance_variable(nm) }

def not_a_proc(x)
  5.singleton_methods((puts "argument"; false), &x)
rescue TypeError => e
  puts "TypeError: #{e.message}"
end
not_a_proc(ARGV.size > 5 ? nil : 5)
