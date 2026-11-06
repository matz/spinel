# the module comes from a required file; the program extends, reads and shadows
require_relative "extend_copy_module_in_lib/m"
class C; extend M; end
puts C.v
def C.v = 2
puts C.v
