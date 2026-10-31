# A later reopening is not a builtin's creation. A new program class in
# that same unplaced file must still be absent until its header runs.
def boot_constants
  p Object, Kernel, Comparable, Integer
end

def load_program_class
  require_relative "class_presence_builtin_reopening_lib"
end

boot_constants
begin
  p LateProgram
rescue NameError
  puts "absent"
end
load_program_class
p LateProgram.new.value
boot_constants
