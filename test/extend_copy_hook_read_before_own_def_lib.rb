# a required file's included hook extends the includer; the program replaces the class method later
require_relative "extend_copy_hook_read_before_own_def_lib/h"
class K; include Tagged; end
puts K.tag
def K.tag = "own"
puts K.tag
