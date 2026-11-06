# a required file's included hook extends; the program's later class method supers into it
require_relative "extend_copy_hook_super_lib/h"
class K; include Tagged; end
puts K.tag
def K.tag = "own+" + super
puts K.tag
