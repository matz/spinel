# A write to the load path with several arguments adds each directory, in the
# order CRuby's path takes them (--load-model=graph). unshift and prepend put
# their arguments in front as written (the first is searched first), push and
# append put them behind the path, a chain of << adds one after the other, and
# concat adds the elements of its array literals. A directory that does not
# exist adds none.
$LOAD_PATH.unshift File.join(__dir__, "load_path_args/u2"), File.join(__dir__, "load_path_args/nowhere"), File.join(__dir__, "load_path_args/u1")
require "f"
p LP_F
$LOAD_PATH.push File.join(__dir__, "load_path_args/p1"), File.join(__dir__, "load_path_args/p2")
require "g"
p LP_G
$LOAD_PATH << File.join(__dir__, "load_path_args/q1") << File.join(__dir__, "load_path_args/q2")
require "h"
p LP_H
$LOAD_PATH.concat([File.join(__dir__, "load_path_args/c2")], [File.join(__dir__, "load_path_args/c1")])
require "i"
p LP_I
