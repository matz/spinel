# a required file's method reads the extended copy; the program shadows it between two calls
require_relative "extend_copy_read_in_lib_method/l"
puts g
def C.v = 2
puts g
