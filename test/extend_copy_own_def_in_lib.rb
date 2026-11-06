# the shadowing def is in a file required after the first read
module M; def v = 1; end
class C; extend M; end
puts C.v
require_relative "extend_copy_own_def_in_lib/d"
puts C.v
