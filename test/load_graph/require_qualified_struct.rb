def setup = require_relative("require_qualified_struct_lib")
setup
p Outer::Lib.new(4).a
