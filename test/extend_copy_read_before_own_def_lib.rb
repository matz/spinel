# a required file extends; the program reads the copy, then shadows it
require_relative "extend_copy_read_before_own_def_lib/c"
puts C.v
def C.v = 2
puts C.v
